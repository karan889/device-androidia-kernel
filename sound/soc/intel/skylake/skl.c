/*
 *  skl.c - Implementation of ASoC Intel SKL HD Audio driver
 *
 *  Copyright (C) 2014-2015 Intel Corp
 *  Author: Jeeja KP <jeeja.kp@intel.com>
 *
 *  Derived mostly from Intel HDA driver with following copyrights:
 *  Copyright (c) 2004 Takashi Iwai <tiwai@suse.de>
 *                     PeiSen Hou <pshou@realtek.com.tw>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <sound/pcm.h>
#include "../common/sst-acpi.h"
#include <sound/hda_register.h>
#include <sound/hdaudio.h>
#include <sound/hda_i915.h>
#include <sound/compress_driver.h>
#include "skl.h"
#include "skl-sst-dsp.h"
#include "skl-sst-ipc.h"
#include "skl-topology.h"

static struct skl_machine_pdata skl_dmic_data;

/*
 * initialize the PCI registers
 */
static void skl_update_pci_byte(struct pci_dev *pci, unsigned int reg,
			    unsigned char mask, unsigned char val)
{
	unsigned char data;

	pci_read_config_byte(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_byte(pci, reg, data);
}

static void skl_init_pci(struct skl *skl)
{
	struct hdac_ext_bus *ebus = &skl->ebus;

	/*
	 * Clear bits 0-2 of PCI register TCSEL (at offset 0x44)
	 * TCSEL == Traffic Class Select Register, which sets PCI express QOS
	 * Ensuring these bits are 0 clears playback static on some HD Audio
	 * codecs.
	 * The PCI register TCSEL is defined in the Intel manuals.
	 */
	dev_dbg(ebus_to_hbus(ebus)->dev, "Clearing TCSEL\n");
	skl_update_pci_byte(skl->pci, AZX_PCIREG_TCSEL, 0x07, 0);
}

static void update_pci_dword(struct pci_dev *pci,
			unsigned int reg, u32 mask, u32 val)
{
	u32 data = 0;

	pci_read_config_dword(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_dword(pci, reg, data);
}

/*
 * skl_enable_miscbdcge - enable/dsiable CGCTL.MISCBDCGE bits
 *
 * @dev: device pointer
 * @enable: enable/disable flag
 */
static void skl_enable_miscbdcge(struct device *dev, bool enable)
{
	struct pci_dev *pci = to_pci_dev(dev);
	u32 val;

	val = enable ? AZX_CGCTL_MISCBDCGE_MASK : 0;

	update_pci_dword(pci, AZX_PCIREG_CGCTL, AZX_CGCTL_MISCBDCGE_MASK, val);
}

/*
 * While performing reset, controller may not come back properly causing
 * issues, so recommendation is to set CGCTL.MISCBDCGE to 0 then do reset
 * (init chip) and then again set CGCTL.MISCBDCGE to 1
 */
static int skl_init_chip(struct hdac_bus *bus, bool full_reset)
{
	int ret;

	skl_enable_miscbdcge(bus->dev, false);
	ret = snd_hdac_bus_init_chip(bus, full_reset);
	skl_enable_miscbdcge(bus->dev, true);

	return ret;
}

void skl_update_d0i3c(struct device *dev, bool enable)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	u8 reg;
	int timeout = 50;

	reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	/* Do not write to D0I3C until command in progress bit is cleared */
	while ((reg & AZX_REG_VS_D0I3C_CIP) && --timeout) {
		udelay(10);
		reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	}

	/* Highly unlikely. But if it happens, flag error explicitly */
	if (!timeout) {
		dev_err(bus->dev, "Before D0I3C update: D0I3C CIP timeout\n");
		return;
	}

	if (enable)
		reg = reg | AZX_REG_VS_D0I3C_I3;
	else
		reg = reg & (~AZX_REG_VS_D0I3C_I3);

	snd_hdac_chip_writeb(bus, VS_D0I3C, reg);

	timeout = 50;
	/* Wait for cmd in progress to be cleared before exiting the function */
	reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	while ((reg & AZX_REG_VS_D0I3C_CIP) && --timeout) {
		udelay(10);
		reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	}

	/* Highly unlikely. But if it happens, flag error explicitly */
	if (!timeout) {
		dev_err(bus->dev, "After D0I3C update: D0I3C CIP timeout\n");
		return;
	}

	dev_dbg(bus->dev, "D0I3C register = 0x%x\n",
			snd_hdac_chip_readb(bus, VS_D0I3C));
}

static void skl_get_total_bytes_transferred(struct hdac_stream *hstr)
{
	int pos, prev_pos, no_of_bytes;

	prev_pos = hstr->curr_pos % hstr->stream->runtime->buffer_size;
	pos = snd_hdac_stream_get_pos_posbuf(hstr);

	if (pos < prev_pos)
		no_of_bytes = (hstr->stream->runtime->buffer_size - prev_pos) +  pos;
	else
		no_of_bytes = pos - prev_pos;

	hstr->curr_pos += no_of_bytes;
}

/*
 * skl_dum_set - Set the DUM bit in EM2 register to fix the IP bug
 * of incorrect postion reporting for capture stream.
 */
static void skl_dum_set(struct hdac_ext_bus *ebus)
{
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	u32 reg;
	u8 val;

	/*
	 * For the DUM bit to be set, CRST needs to be out of reset state
	 */
	val = snd_hdac_chip_readb(bus, GCTL) & AZX_GCTL_RESET;
	if (!val) {
		skl_enable_miscbdcge(bus->dev, false);
		snd_hdac_bus_exit_link_reset(bus);
		skl_enable_miscbdcge(bus->dev, true);
	}
	/*
	 * Set the DUM bit in EM2 register to fix the IP bug of incorrect
	 * postion reporting for capture stream.
	 */
	reg  = snd_hdac_chip_readl(bus, VS_EM2);
	snd_hdac_chip_writel(bus, VS_EM2, (reg | AZX_EM2_DUM_MASK));
}

/* called from IRQ */
static void skl_stream_update(struct hdac_bus *bus, struct hdac_stream *hstr)
{
	if (hstr->substream)
		snd_pcm_period_elapsed(hstr->substream);
	else if (hstr->stream) {
		skl_get_total_bytes_transferred(hstr);
		snd_compr_fragment_elapsed(hstr->stream);
	}
}

static irqreturn_t skl_interrupt(int irq, void *dev_id)
{
	struct hdac_ext_bus *ebus = dev_id;
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	u32 status;
	u32 mask, int_enable;
	int ret = IRQ_NONE;

	if (!pm_runtime_active(bus->dev))
		return ret;

	spin_lock(&bus->reg_lock);

	status = snd_hdac_chip_readl(bus, INTSTS);
	if (status == 0 || status == 0xffffffff) {
		spin_unlock(&bus->reg_lock);
		return ret;
	}

	/* clear rirb int */
	status = snd_hdac_chip_readb(bus, RIRBSTS);
	if (status & RIRB_INT_MASK) {
		if (status & RIRB_INT_RESPONSE)
			snd_hdac_bus_update_rirb(bus);
		snd_hdac_chip_writeb(bus, RIRBSTS, RIRB_INT_MASK);
	}

	mask = (0x1 << ebus->num_streams) - 1;

	status = snd_hdac_chip_readl(bus, INTSTS);
	status &= mask;
	if (status) {
		/* Disable stream interrupts; Re-enable in bottom half */
		int_enable = snd_hdac_chip_readl(bus, INTCTL);
		snd_hdac_chip_writel(bus, INTCTL, (int_enable & (~mask)));
		ret = IRQ_WAKE_THREAD;
	} else
		ret = IRQ_HANDLED;

	spin_unlock(&bus->reg_lock);
	return ret;

}

static irqreturn_t skl_threaded_handler(int irq, void *dev_id)
{
	struct hdac_ext_bus *ebus = dev_id;
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	u32 status;
	u32 int_enable;
	u32 mask;
	unsigned long flags;

	status = snd_hdac_chip_readl(bus, INTSTS);

	snd_hdac_bus_handle_stream_irq(bus, status, skl_stream_update);

	/* Re-enable stream interrupts */
	mask = (0x1 << ebus->num_streams) - 1;
	spin_lock_irqsave(&bus->reg_lock, flags);
	int_enable = snd_hdac_chip_readl(bus, INTCTL);
	snd_hdac_chip_writel(bus, INTCTL, (int_enable | mask));
	spin_unlock_irqrestore(&bus->reg_lock, flags);
	return IRQ_HANDLED;
}

static int skl_acquire_irq(struct hdac_ext_bus *ebus, int do_disconnect)
{
	struct skl *skl = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	int ret;

	ret = request_threaded_irq(skl->pci->irq, skl_interrupt,
			skl_threaded_handler,
			IRQF_SHARED,
			KBUILD_MODNAME, ebus);
	if (ret) {
		dev_err(bus->dev,
			"unable to grab IRQ %d, disabling device\n",
			skl->pci->irq);
		return ret;
	}

	bus->irq = skl->pci->irq;
	pci_intx(skl->pci, 1);

	return 0;
}

static int skl_suspend_late(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct skl *skl = ebus_to_skl(ebus);

	return skl_suspend_late_dsp(skl);
}

#ifdef CONFIG_PM
static int _skl_suspend(struct hdac_ext_bus *ebus)
{
	struct skl *skl = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct pci_dev *pci = to_pci_dev(bus->dev);
	int ret;

	snd_hdac_ext_bus_link_power_down_all(ebus);

	ret = skl_suspend_dsp(skl);
	if (ret < 0)
		return ret;

	snd_hdac_bus_stop_chip(bus);
	update_pci_dword(pci, AZX_PCIREG_PGCTL,
		AZX_PGCTL_LSRMD_MASK, AZX_PGCTL_LSRMD_MASK);
	skl_enable_miscbdcge(bus->dev, false);
	snd_hdac_bus_enter_link_reset(bus);
	skl_enable_miscbdcge(bus->dev, true);
	skl_cleanup_resources(skl);

	return 0;
}

static int _skl_resume(struct hdac_ext_bus *ebus)
{
	struct skl *skl = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	skl_init_pci(skl);
	skl_init_chip(bus, true);

	return skl_resume_dsp(skl);
}
#endif

#ifdef CONFIG_PM_SLEEP
/*
 * power management
 */
static int skl_suspend(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct skl *skl  = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	int ret = 0;

	/*
	 * Do not suspend if streams which are marked ignore suspend are
	 * running, we need to save the state for these and continue
	 */
	if (skl->supend_active) {
		/* turn off the links and stop the CORB/RIRB DMA if it is On */
		snd_hdac_ext_bus_link_power_down_all(ebus);

		if (ebus->cmd_dma_state)
			snd_hdac_bus_stop_cmd_io(&ebus->bus);

		enable_irq_wake(bus->irq);
		pci_save_state(pci);
	} else {
		ret = _skl_suspend(ebus);
		if (ret < 0)
			return ret;
		skl->skl_sst->fw_loaded = false;
	}

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = snd_hdac_display_power(bus, false);
		if (ret < 0)
			dev_err(bus->dev,
				"Cannot turn OFF display power on i915\n");
	}

	return ret;
}

static int skl_resume(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct skl *skl  = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_ext_link *hlink = NULL;
	int ret = 0;

	/* Turned OFF in HDMI codec driver after codec reconfiguration */
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = snd_hdac_display_power(bus, true);
		if (ret < 0) {
			dev_err(bus->dev,
				"Cannot turn on display power on i915\n");
			return ret;
		}
	}

	/*
	 * resume only when we are not in suspend active, otherwise need to
	 * restore the device
	 */
	if (skl->supend_active) {
		pci_restore_state(pci);
		snd_hdac_ext_bus_link_power_up_all(ebus);
		disable_irq_wake(bus->irq);
		/*
		 * turn On the links which are On before active suspend
		 * and start the CORB/RIRB DMA if On before
		 * active suspend.
		 */
		list_for_each_entry(hlink, &ebus->hlink_list, list) {
			if (hlink->ref_count)
				snd_hdac_ext_bus_link_power_up(hlink);
		}

		if (ebus->cmd_dma_state)
			snd_hdac_bus_init_cmd_io(&ebus->bus);
	} else {
		ret = _skl_resume(ebus);

		/* turn off the links which are off before suspend */
		list_for_each_entry(hlink, &ebus->hlink_list, list) {
			if (!hlink->ref_count)
				snd_hdac_ext_bus_link_power_down(hlink);
		}

		if (!ebus->cmd_dma_state)
			snd_hdac_bus_stop_cmd_io(&ebus->bus);
	}

	return ret;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM
static int skl_runtime_suspend(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	dev_dbg(bus->dev, "in %s\n", __func__);

	return _skl_suspend(ebus);
}

static int skl_runtime_resume(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	dev_dbg(bus->dev, "in %s\n", __func__);

	return _skl_resume(ebus);
}
#endif /* CONFIG_PM */

static const struct dev_pm_ops skl_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(skl_suspend, skl_resume)
	SET_RUNTIME_PM_OPS(skl_runtime_suspend, skl_runtime_resume, NULL)
	.suspend_late = skl_suspend_late,
};

/*
 * destructor
 */
static int skl_free(struct hdac_ext_bus *ebus)
{
	struct skl *skl  = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	skl->init_done = 0; /* to be sure */

	snd_hdac_ext_stop_streams(ebus);

	if (bus->irq >= 0)
		free_irq(bus->irq, (void *)ebus);
	snd_hdac_bus_free_stream_pages(bus);
	snd_hdac_stream_free_all(ebus);
	snd_hdac_link_free_all(ebus);

	if (bus->remap_addr)
		iounmap(bus->remap_addr);

	pci_release_regions(skl->pci);
	pci_disable_device(skl->pci);

	snd_hdac_ext_bus_exit(ebus);

	cancel_work_sync(&skl->probe_work);
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
		snd_hdac_i915_exit(&ebus->bus);

	return 0;
}

static int skl_find_mchine(struct skl *skl, void *driver_data)
{
	struct sst_acpi_mach *mach = driver_data;
	struct hdac_bus *bus = ebus_to_hbus(&skl->ebus);

	if (IS_ENABLED(CONFIG_SND_SOC_RT700) ||
	    IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA))
		goto out;

	mach = sst_acpi_find_machine(mach);
	if (mach == NULL) {
		dev_err(bus->dev, "No matching machine driver found\n");
		return -ENODEV;
	}

out:

	skl->fw_name = mach->fw_filename;
	skl->mach = mach;
	if (mach->pdata) {
		skl->use_tplg_pcm =
			((struct skl_machine_pdata *)mach->pdata)->use_tplg_pcm;
	}

	return 0;
}

static int skl_machine_device_register(struct skl *skl)
{
	struct hdac_bus *bus = ebus_to_hbus(&skl->ebus);
	struct sst_acpi_mach *mach = skl->mach;
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc(mach->drv_name, -1);
	if (pdev == NULL) {
		dev_err(bus->dev, "platform device alloc failed\n");
		return -EIO;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		dev_err(bus->dev, "failed to add machine device\n");
		platform_device_put(pdev);
		return -EIO;
	}

	if (mach->pdata)
		dev_set_drvdata(&pdev->dev, mach->pdata);

	skl->i2s_dev = pdev;

	return 0;
}

static void skl_machine_device_unregister(struct skl *skl)
{
	if (skl->i2s_dev)
		platform_device_unregister(skl->i2s_dev);
}

static int skl_dmic_device_register(struct skl *skl)
{
	struct hdac_bus *bus = ebus_to_hbus(&skl->ebus);
	struct platform_device *pdev;
	int ret;

	/* SKL has one dmic port, so allocate dmic device for this */
	pdev = platform_device_alloc("dmic-codec", -1);
	if (!pdev) {
		dev_err(bus->dev, "failed to allocate dmic device\n");
		return -ENOMEM;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		dev_err(bus->dev, "failed to add dmic device: %d\n", ret);
		platform_device_put(pdev);
		return ret;
	}
	skl->dmic_dev = pdev;

	return 0;
}

static void skl_dmic_device_unregister(struct skl *skl)
{
	if (skl->dmic_dev)
		platform_device_unregister(skl->dmic_dev);
}

/*
 * Probe the given codec address
 */
static int probe_codec(struct hdac_ext_bus *ebus, int addr)
{
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	unsigned int res = -1;

	mutex_lock(&bus->cmd_mutex);
	snd_hdac_bus_send_cmd(bus, cmd);
	snd_hdac_bus_get_response(bus, addr, &res);
	mutex_unlock(&bus->cmd_mutex);
	if (res == -1)
		return -EIO;
	dev_dbg(bus->dev, "codec #%d probed OK\n", addr);

	return snd_hdac_ext_bus_device_init(ebus, addr);
}

/* Codec initialization */
static void __maybe_unused skl_codec_create(struct hdac_ext_bus *ebus)
{
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	int c, max_slots;

	max_slots = HDA_MAX_CODECS;

	/* First try to probe all given codec slots */
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c))) {
			if (probe_codec(ebus, c) < 0) {
				/*
				 * Some BIOSen give you wrong codec addresses
				 * that don't exist
				 */
				dev_warn(bus->dev,
					 "Codec #%d probe error; disabling it...\n", c);
				bus->codec_mask &= ~(1 << c);
				/*
				 * More badly, accessing to a non-existing
				 * codec often screws up the controller bus,
				 * and disturbs the further communications.
				 * Thus if an error occurs during probing,
				 * better to reset the controller bus to get
				 * back to the sanity state.
				 */
				snd_hdac_bus_stop_chip(bus);
				skl_init_chip(bus, true);
			}
		}
	}
}

static const struct hdac_bus_ops bus_core_ops = {
	.command = snd_hdac_bus_send_cmd,
	.get_response = snd_hdac_bus_get_response,
};

static int skl_i915_init(struct hdac_bus *bus)
{
	int err;

	/*
	 * The HDMI codec is in GPU so we need to ensure that it is powered
	 * up and ready for probe
	 */
	err = snd_hdac_i915_init(bus);
	if (err < 0)
		return err;

	err = snd_hdac_display_power(bus, true);
	if (err < 0)
		dev_err(bus->dev, "Cannot turn on display power on i915\n");

	return err;
}

static void skl_probe_work(struct work_struct *work)
{
	struct skl *skl = container_of(work, struct skl, probe_work);
	struct hdac_ext_bus *ebus = &skl->ebus;
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_ext_link *hlink = NULL;
	int err;

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		err = skl_i915_init(bus);
		if (err < 0)
			return;
	}

	err = skl_init_chip(bus, true);
	if (err < 0) {
		dev_err(bus->dev, "Init chip failed with err: %d\n", err);
		goto out_err;
	}

	/* codec detection */
	if (!bus->codec_mask)
		dev_info(bus->dev, "no hda codecs found!\n");

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	/* create codec instances */
	skl_codec_create(ebus);
#endif

	/* register platform dai and controls */
	err = skl_platform_register(bus->dev);
	if (err < 0)
		return;

	if (bus->ppcap) {
		err = skl_machine_device_register(skl);
		if (err < 0) {
			dev_err(bus->dev,
				"machine device register failed with err: %d\n",
				err);
			goto out_err;
		}
	}

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		err = snd_hdac_display_power(bus, false);
		if (err < 0) {
			dev_err(bus->dev, "Cannot turn off display power on i915\n");
			skl_machine_device_unregister(skl);
			return;
		}
	}

	/*
	 * we are done probing so decrement link counts
	 */
	list_for_each_entry(hlink, &ebus->hlink_list, list)
		snd_hdac_ext_bus_link_put(ebus, hlink);

	/* configure PM */
	pm_runtime_put_noidle(bus->dev);
	pm_runtime_allow(bus->dev);
	skl->init_done = 1;

	return;

out_err:
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
		err = snd_hdac_display_power(bus, false);
}

/*
 * constructor
 */
static int skl_create(struct pci_dev *pci,
		      const struct hdac_io_ops *io_ops,
		      struct skl **rskl)
{
	struct skl *skl;
	struct hdac_ext_bus *ebus;

	int err;

	*rskl = NULL;

	err = pci_enable_device(pci);
	if (err < 0)
		return err;

	skl = devm_kzalloc(&pci->dev, sizeof(*skl), GFP_KERNEL);
	if (!skl) {
		pci_disable_device(pci);
		return -ENOMEM;
	}
	ebus = &skl->ebus;
	snd_hdac_ext_bus_init(ebus, &pci->dev, &bus_core_ops, io_ops);
	ebus->bus.use_posbuf = 1;
	skl->pci = pci;
	INIT_WORK(&skl->probe_work, skl_probe_work);

	ebus->bus.bdl_pos_adj = 0;

	*rskl = skl;

	return 0;
}

static int skl_first_init(struct hdac_ext_bus *ebus)
{
	struct skl *skl = ebus_to_skl(ebus);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct pci_dev *pci = skl->pci;
	int err;
	unsigned short gcap;
	int cp_streams, pb_streams, start_idx;

	err = pci_request_regions(pci, "Skylake HD audio");
	if (err < 0)
		return err;

	bus->addr = pci_resource_start(pci, 0);
	bus->remap_addr = pci_ioremap_bar(pci, 0);
	if (bus->remap_addr == NULL) {
		dev_err(bus->dev, "ioremap error\n");
		return -ENXIO;
	}

	skl_init_chip(bus, true);

	snd_hdac_bus_parse_capabilities(bus);

	if (skl_acquire_irq(ebus, 0) < 0)
		return -EBUSY;

	pci_set_master(pci);
	synchronize_irq(bus->irq);

	gcap = snd_hdac_chip_readw(bus, GCAP);
	dev_dbg(bus->dev, "chipset global capabilities = 0x%x\n", gcap);

	/* allow 64bit DMA address if supported by H/W */
	if (!dma_set_mask(bus->dev, DMA_BIT_MASK(64))) {
		dma_set_coherent_mask(bus->dev, DMA_BIT_MASK(64));
	} else {
		dma_set_mask(bus->dev, DMA_BIT_MASK(32));
		dma_set_coherent_mask(bus->dev, DMA_BIT_MASK(32));
	}

	/* read number of streams from GCAP register */
	cp_streams = (gcap >> 8) & 0x0f;
	pb_streams = (gcap >> 12) & 0x0f;

	if (!pb_streams && !cp_streams)
		return -EIO;

	ebus->num_streams = cp_streams + pb_streams;

	/* initialize streams */
	snd_hdac_ext_stream_init_all
		(ebus, 0, cp_streams, SNDRV_PCM_STREAM_CAPTURE);
	start_idx = cp_streams;
	snd_hdac_ext_stream_init_all
		(ebus, start_idx, pb_streams, SNDRV_PCM_STREAM_PLAYBACK);

	err = snd_hdac_bus_alloc_stream_pages(bus);
	if (err < 0)
		return err;

	/* initialize chip */
	skl_init_pci(skl);

	skl_dum_set(ebus);

	return skl_init_chip(bus, true);
}

static int skl_probe(struct pci_dev *pci,
		     const struct pci_device_id *pci_id)
{
	struct skl *skl;
	struct hdac_ext_bus *ebus = NULL;
	struct hdac_bus *bus = NULL;
	const struct firmware __maybe_unused *nhlt_fw = NULL;
	int err;

	/* we use ext core ops, so provide NULL for ops here */
	err = skl_create(pci, NULL, &skl);
	if (err < 0)
		return err;

	ebus = &skl->ebus;
	bus = ebus_to_hbus(ebus);

	err = skl_first_init(ebus);
	if (err < 0)
		goto out_free;

	skl->pci_id = pci->device;

	device_disable_async_suspend(bus->dev);

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	skl->nhlt_version = skl_get_nhlt_version(bus->dev);
	skl->nhlt = skl_nhlt_init(bus->dev);

	if (skl->nhlt == NULL) {
		err = -ENODEV;
		goto out_free;
	}

	err = skl_nhlt_create_sysfs(skl);
	if (err < 0)
		goto out_nhlt_free;

	skl_nhlt_update_topology_bin(skl);

#else
	if (request_firmware(&nhlt_fw, "intel/nhlt_blob.bin", bus->dev)) {
		dev_err(bus->dev, "Request nhlt fw failed, continuing..\n");
		goto nhlt_continue;
	}

	skl->nhlt = devm_kzalloc(&pci->dev, nhlt_fw->size, GFP_KERNEL);
	if (skl->nhlt == NULL)
		return -ENOMEM;
	memcpy(skl->nhlt, nhlt_fw->data, nhlt_fw->size);
	release_firmware(nhlt_fw);

nhlt_continue:
#endif
	pci_set_drvdata(skl->pci, ebus);

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	skl_dmic_data.dmic_num = skl_get_dmic_geo(skl);
#endif

	/* check if dsp is there */
	if (bus->ppcap) {
		err = skl_find_mchine(skl, (void *)pci_id->driver_data);
		if (err < 0)
			goto out_nhlt_free;

		err = skl_init_dsp(skl);
		if (err < 0) {
			dev_dbg(bus->dev, "error failed to register dsp\n");
			goto out_nhlt_free;
		}
		skl->skl_sst->enable_miscbdcge = skl_enable_miscbdcge;

	}
	if (bus->mlcap)
		snd_hdac_ext_bus_get_ml_capabilities(ebus);

	snd_hdac_bus_stop_chip(bus);

	/* create device for soc dmic */
	err = skl_dmic_device_register(skl);
	if (err < 0)
		goto out_dsp_free;

	schedule_work(&skl->probe_work);

	return 0;

out_dsp_free:
	skl_free_dsp(skl);
out_nhlt_free:
	skl_nhlt_free(skl->nhlt);
out_free:
	skl_free(ebus);

	return err;
}

static void skl_shutdown(struct pci_dev *pci)
{
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_stream *s;
	struct hdac_ext_stream *stream;
	struct skl *skl;

	if (ebus == NULL)
		return;

	skl = ebus_to_skl(ebus);

	if (!skl->init_done)
		return;

	snd_hdac_ext_stop_streams(ebus);
	list_for_each_entry(s, &bus->stream_list, list) {
		stream = stream_to_hdac_ext_stream(s);
		snd_hdac_ext_stream_decouple(ebus, stream, false);
	}

	snd_hdac_bus_stop_chip(bus);
}

static void skl_remove(struct pci_dev *pci)
{
	struct hdac_ext_bus *ebus = pci_get_drvdata(pci);
	struct skl *skl = ebus_to_skl(ebus);

	skl_delete_notify_kctl_list(skl->skl_sst);
	release_firmware(skl->tplg);

	pm_runtime_get_noresume(&pci->dev);

	/* codec removal, invoke bus_device_remove */
	snd_hdac_ext_bus_device_remove(ebus);

	skl->debugfs = NULL;
	skl_platform_unregister(&pci->dev);
	skl_free_dsp(skl);
	skl_machine_device_unregister(skl);
	skl_dmic_device_unregister(skl);
	skl_nhlt_remove_sysfs(skl);
	skl_nhlt_free(skl->nhlt);
	skl_free(ebus);
	dev_set_drvdata(&pci->dev, NULL);
}

static struct sst_codecs skl_codecs = {
	.num_codecs = 1,
	.codecs = {"10508825"}
};

static struct sst_codecs kbl_codecs = {
	.num_codecs = 1,
	.codecs = {"10508825"}
};

static struct sst_codecs bxt_codecs = {
	.num_codecs = 1,
	.codecs = {"MX98357A"}
};

static struct sst_codecs kbl_poppy_codecs = {
	.num_codecs = 1,
	.codecs = {"10EC5663"}
};

static struct sst_codecs kbl_5663_5514_codecs = {
	.num_codecs = 2,
	.codecs = {"10EC5663", "10EC5514"}
};


static struct sst_acpi_mach sst_skl_devdata[] = {
	{
		.id = "INT343A",
		.drv_name = "skl_alc286s_i2s",
		.fw_filename = "intel/dsp_fw_release.bin",
	},
	{
		.id = "INT343B",
		.drv_name = "skl_n88l25_s4567",
		.fw_filename = "intel/dsp_fw_release.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &skl_codecs,
		.pdata = &skl_dmic_data
	},
	{
		.id = "MX98357A",
		.drv_name = "skl_n88l25_m98357a",
		.fw_filename = "intel/dsp_fw_release.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &skl_codecs,
		.pdata = &skl_dmic_data
	},
	{}
};

static struct sst_acpi_mach sst_bxtp_devdata[] = {
	{
		.id = "INT343A",
		.drv_name = "bxt_alc298s_i2s",
		.fw_filename = "intel/dsp_fw_bxtn.bin",
	},
	{
		.id = "DLGS7219",
		.drv_name = "bxt_da7219_max98357a_i2s",
		.fw_filename = "intel/dsp_fw_bxtn.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &bxt_codecs,
	},
	{
		.id = "INT34C3",
		.drv_name = "bxt_tdf8532",
		.fw_filename = "intel/dsp_fw_bxtn.bin",
	},
	{}
};

static struct sst_acpi_mach sst_kbl_devdata[] = {
	{
		.id = "INT343A",
		.drv_name = "kbl_alc286s_i2s",
		.fw_filename = "intel/dsp_fw_kbl.bin",
	},
	{
		.id = "INT343B",
		.drv_name = "kbl_n88l25_s4567",
		.fw_filename = "intel/dsp_fw_kbl.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &kbl_codecs,
		.pdata = &skl_dmic_data
	},
	{
		.id = "MX98357A",
		.drv_name = "kbl_n88l25_m98357a",
		.fw_filename = "intel/dsp_fw_kbl.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &kbl_codecs,
		.pdata = &skl_dmic_data
	},
	{
		.id = "MX98927",
		.drv_name = "kbl_r5514_5663_max",
		.fw_filename = "intel/dsp_fw_kbl.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &kbl_5663_5514_codecs,
		.pdata = &skl_dmic_data
	},
	{
		.id = "MX98927",
		.drv_name = "kbl_rt5663_m98927",
		.fw_filename = "intel/dsp_fw_kbl.bin",
		.machine_quirk = sst_acpi_codec_list,
		.quirk_data = &kbl_poppy_codecs,
		.pdata = &skl_dmic_data
	},
	{
		.id = "10EC5663",
		.drv_name = "kbl_rt5663",
		.fw_filename = "intel/dsp_fw_kbl.bin",
	},

	{}
};

static struct sst_acpi_mach sst_glk_devdata[] = {
	{
		.id = "INT343A",
		.drv_name = "glk_alc298s_i2s",
		.fw_filename = "intel/dsp_fw_glk.bin",
	},
	{}
};

static const struct sst_acpi_mach sst_cnl_devdata[] = {
#if !IS_ENABLED(CONFIG_SND_SOC_RT700)
	{
		.id = "INT34C2",
		.drv_name = "cnl_rt274",
		.fw_filename = "intel/dsp_fw_cnl.bin",
	},
#else
	{
		.drv_name = "cnl_rt700",
		.fw_filename = "intel/dsp_fw_cnl.bin",
	},
#endif
};

static struct sst_acpi_mach sst_icl_devdata[] = {
#if IS_ENABLED(CONFIG_SND_SOC_RT700)
	{ "dummy", "icl_rt700", "intel/dsp_fw_icl.bin", NULL, NULL, NULL },
#elif IS_ENABLED(CONFIG_SND_SOC_WM5110)
	{ "dummy", "icl_wm8281", "intel/dsp_fw_icl.bin", NULL, NULL, NULL },
#else
	{ "dummy", "icl_rt274", "intel/dsp_fw_icl.bin", NULL, NULL, NULL },
#endif
	{}
};

/* PCI IDs */
static const struct pci_device_id skl_ids[] = {
	/* Sunrise Point-LP */
	{ PCI_DEVICE(0x8086, 0x9d70),
		.driver_data = (unsigned long)&sst_skl_devdata},
	/* BXT-P */
	{ PCI_DEVICE(0x8086, 0x5a98),
		.driver_data = (unsigned long)&sst_bxtp_devdata},
	/* KBL */
	{ PCI_DEVICE(0x8086, 0x9D71),
		.driver_data = (unsigned long)&sst_kbl_devdata},
	/* GLK */
	{ PCI_DEVICE(0x8086, 0x3198),
		.driver_data = (unsigned long)&sst_glk_devdata},
	/* CNL */
	{ PCI_DEVICE(0x8086, 0x9dc8),
		.driver_data = (unsigned long)&sst_cnl_devdata},
	/* ICL */
	{ PCI_DEVICE(0x8086, 0x34c8),
		.driver_data = (unsigned long)&sst_icl_devdata},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, skl_ids);

/* pci_driver definition */
static struct pci_driver skl_driver = {
	.name = KBUILD_MODNAME,
	.id_table = skl_ids,
	.probe = skl_probe,
	.remove = skl_remove,
	.shutdown = skl_shutdown,
	.driver = {
		.pm = &skl_pm,
	},
};
module_pci_driver(skl_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Skylake ASoC HDA driver");
