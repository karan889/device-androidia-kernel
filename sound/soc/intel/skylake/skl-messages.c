/*
 *  skl-message.c - HDA DSP interface for FW registration, Pipe and Module
 *  configurations
 *
 *  Copyright (C) 2015 Intel Corp
 *  Author:Rafal Redzimski <rafal.f.redzimski@intel.com>
 *	   Jeeja KP <jeeja.kp@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/pci.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <linux/delay.h>
#include "skl-sst-dsp.h"
#include "cnl-sst-dsp.h"
#include "skl-sst-ipc.h"
#include "skl.h"
#include "../common/sst-dsp.h"
#include "../common/sst-dsp-priv.h"
#include "skl-topology.h"
#include "skl-tplg-interface.h"
#include <linux/sdw/sdw_cnl.h>
#include <linux/sdw_bus.h>
#include <asm/set_memory.h>

#define ASRC_MODE_UPLINK	2
#define ASRC_MODE_DOWNLINK	1
#define SKL_ENABLE_ALL_CHANNELS  0xffffffff

static int skl_alloc_dma_buf(struct device *dev,
		struct snd_dma_buffer *dmab, size_t size)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	if (!bus)
		return -ENODEV;

	return  bus->io_ops->dma_alloc_pages(bus, SNDRV_DMA_TYPE_DEV, size, dmab);
}

static int skl_free_dma_buf(struct device *dev, struct snd_dma_buffer *dmab)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	if (!bus)
		return -ENODEV;

	bus->io_ops->dma_free_pages(bus, dmab);

	return 0;
}

#define ENABLE_LOGS		6
#define FW_LOGGING_AGING_TIMER_PERIOD 100
#define FW_LOG_FIFO_FULL_TIMER_PERIOD 100

/* set firmware logging state via IPC */
int skl_dsp_enable_logging(struct sst_generic_ipc *ipc, int core, int enable)
{
	struct skl_log_state_msg log_msg;
	struct skl_ipc_large_config_msg msg = {0};
	int ret = 0;

	log_msg.aging_timer_period = FW_LOGGING_AGING_TIMER_PERIOD;
	log_msg.fifo_full_timer_period = FW_LOG_FIFO_FULL_TIMER_PERIOD;

	log_msg.core_mask = (1 << core);
	log_msg.logs_core[core].enable = enable;
	log_msg.logs_core[core].priority = ipc->dsp->trace_wind.log_priority;

	msg.large_param_id = ENABLE_LOGS;
	msg.param_data_size = sizeof(log_msg);

	ret = skl_ipc_set_large_config(ipc, &msg, (u32 *)&log_msg);

	return ret;
}

#define SYSTEM_TIME		20

/* set system time to DSP via IPC */
int skl_dsp_set_system_time(struct skl_sst *skl_sst)
{
	struct sst_generic_ipc *ipc = &skl_sst->ipc;
	struct SystemTime sys_time_msg;
	struct skl_ipc_large_config_msg msg = {0};
	struct timeval tv;
	u64 sys_time;
	u64 mask = 0x00000000FFFFFFFF;
	int ret;

	do_gettimeofday(&tv);

	/* DSP firmware expects UTC time in micro seconds */
	sys_time = tv.tv_sec*1000*1000 + tv.tv_usec;
	sys_time_msg.val_l = sys_time & mask;
	sys_time_msg.val_u = (sys_time & (~mask)) >> 32;

	msg.large_param_id = SYSTEM_TIME;
	msg.param_data_size = sizeof(sys_time_msg);

	ret = skl_ipc_set_large_config(ipc, &msg, (u32 *)&sys_time_msg);
	return ret;
}

#define SKL_ASTATE_PARAM_ID	4

void skl_dsp_set_astate_cfg(struct skl_sst *ctx, u32 cnt, void *data)
{
	struct skl_ipc_large_config_msg	msg = {0};

	msg.large_param_id = SKL_ASTATE_PARAM_ID;
	msg.param_data_size = (cnt * sizeof(struct skl_astate_config) +
				sizeof(cnt));

	skl_ipc_set_large_config(&ctx->ipc, &msg, data);
}

#define NOTIFICATION_PARAM_ID 3
#define NOTIFICATION_MASK 0xf

/* disable notfication for underruns/overruns from firmware module */
void skl_dsp_enable_notification(struct skl_sst *ctx, bool enable)
{
	struct notification_mask mask;
	struct skl_ipc_large_config_msg	msg = {0};

	mask.notify = NOTIFICATION_MASK;
	mask.enable = enable;

	msg.large_param_id = NOTIFICATION_PARAM_ID;
	msg.param_data_size = sizeof(mask);

	skl_ipc_set_large_config(&ctx->ipc, &msg, (u32 *)&mask);
}

static int skl_dsp_setup_spib(struct device *dev, unsigned int size,
				int stream_tag, int enable)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_stream *stream = snd_hdac_get_stream(bus,
			SNDRV_PCM_STREAM_PLAYBACK, stream_tag);
	struct hdac_ext_stream *estream;

	if (!stream)
		return -EINVAL;

	estream = stream_to_hdac_ext_stream(stream);
	/* enable/disable SPIB for this hdac stream */
	snd_hdac_ext_stream_spbcap_enable(ebus, enable, stream->index);

	/* set the spib value */
	snd_hdac_ext_stream_set_spib(ebus, estream, size);

	return 0;
}

static int skl_dsp_prepare(struct device *dev, unsigned int format,
						unsigned int size,
						struct snd_dma_buffer *dmab,
						int direction)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct hdac_ext_stream *estream;
	struct hdac_stream *stream;
	struct snd_pcm_substream substream;
	int ret;

	if (!bus)
		return -ENODEV;

	memset(&substream, 0, sizeof(substream));

	substream.stream = direction;

	estream = snd_hdac_ext_stream_assign(ebus, &substream,
					HDAC_EXT_STREAM_TYPE_HOST);
	if (!estream)
		return -ENODEV;

	stream = hdac_stream(estream);

	/* assign decouple host dma channel */
	ret = snd_hdac_dsp_prepare(stream, format, size, dmab);
	if (ret < 0)
		return ret;

	skl_dsp_setup_spib(dev, size, stream->stream_tag, true);

	return stream->stream_tag;
}

static int skl_dsp_trigger(struct device *dev, bool start, int stream_tag,
							int direction)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_stream *stream;
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	if (!bus)
		return -ENODEV;

	stream = snd_hdac_get_stream(bus, direction, stream_tag);
	if (!stream)
		return -EINVAL;

	snd_hdac_dsp_trigger(stream, start);

	return 0;
}

static int skl_dsp_cleanup(struct device *dev, struct snd_dma_buffer *dmab,
				int stream_tag, int direction)
{
	struct hdac_ext_bus *ebus = dev_get_drvdata(dev);
	struct hdac_stream *stream;
	struct hdac_ext_stream *estream;
	struct hdac_bus *bus = ebus_to_hbus(ebus);

	if (!bus)
		return -ENODEV;

	stream = snd_hdac_get_stream(bus, direction, stream_tag);
	if (!stream)
		return -EINVAL;

	estream = stream_to_hdac_ext_stream(stream);
	skl_dsp_setup_spib(dev, 0, stream_tag, false);
	snd_hdac_ext_stream_release(estream, HDAC_EXT_STREAM_TYPE_HOST);

	snd_hdac_dsp_cleanup(stream, dmab);

	return 0;
}

static struct skl_dsp_loader_ops skl_get_loader_ops(void)
{
	struct skl_dsp_loader_ops loader_ops;

	memset(&loader_ops, 0, sizeof(struct skl_dsp_loader_ops));

	loader_ops.alloc_dma_buf = skl_alloc_dma_buf;
	loader_ops.free_dma_buf = skl_free_dma_buf;

	return loader_ops;
};

static struct skl_dsp_loader_ops bxt_get_loader_ops(void)
{
	struct skl_dsp_loader_ops loader_ops;

	memset(&loader_ops, 0, sizeof(loader_ops));

	loader_ops.alloc_dma_buf = skl_alloc_dma_buf;
	loader_ops.free_dma_buf = skl_free_dma_buf;
	loader_ops.prepare = skl_dsp_prepare;
	loader_ops.trigger = skl_dsp_trigger;
	loader_ops.cleanup = skl_dsp_cleanup;

	return loader_ops;
};

static const struct skl_dsp_ops dsp_ops[] = {
	{
		.id = 0x9d70,
		.num_cores = 2,
		.loader_ops = skl_get_loader_ops,
		.init = skl_sst_dsp_init,
		.init_fw = skl_sst_init_fw,
		.cleanup = skl_sst_dsp_cleanup
	},
	{
		.id = 0x9d71,
		.num_cores = 2,
		.loader_ops = skl_get_loader_ops,
		.init = skl_sst_dsp_init,
		.init_fw = skl_sst_init_fw,
		.cleanup = skl_sst_dsp_cleanup
	},
	{
		.id = 0x5a98,
		.num_cores = 2,
		.loader_ops = bxt_get_loader_ops,
		.init = bxt_sst_dsp_init,
		.init_fw = bxt_sst_init_fw,
		.cleanup = bxt_sst_dsp_cleanup
	},
	{
		.id = 0x3198,
		.num_cores = 2,
		.loader_ops = bxt_get_loader_ops,
		.init = bxt_sst_dsp_init,
		.init_fw = bxt_sst_init_fw,
		.cleanup = bxt_sst_dsp_cleanup
	},
	{
		.id = 0x9dc8,
		.num_cores = 4,
		.loader_ops = bxt_get_loader_ops,
		.init = cnl_sst_dsp_init,
		.init_fw = cnl_sst_init_fw,
		.cleanup = cnl_sst_dsp_cleanup
	},
	{
		.id = 0x34c8,
		.num_cores = 4,
		.loader_ops = bxt_get_loader_ops,
		.init = cnl_sst_dsp_init,
		.init_fw = cnl_sst_init_fw,
		.cleanup = cnl_sst_dsp_cleanup
	},
};

static int cnl_sdw_bra_pipe_trigger(struct skl_sst *ctx, bool enable,
				unsigned int mstr_num)
{
	struct bra_conf *bra_data = &ctx->bra_pipe_data[mstr_num];
	int ret;

	if (enable) {

		/* Run CP Pipeline */
		ret = skl_run_pipe(ctx, bra_data->cp_pipe);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: RX run pipeline failed: 0x%x\n", ret);
			goto error;
		}

		/* Run PB Pipeline */
		ret = skl_run_pipe(ctx, bra_data->pb_pipe);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: TX run pipeline failed: 0x%x\n", ret);
			goto error;
		}

	} else {

		/* Stop playback pipeline */
		ret = skl_stop_pipe(ctx, bra_data->pb_pipe);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: TX stop pipeline failed: 0x%x\n", ret);
			goto error;
		}

		/* Stop capture pipeline */
		ret = skl_stop_pipe(ctx, bra_data->cp_pipe);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: RX stop pipeline failed: 0x%x\n", ret);
			goto error;
		}
	}

error:
	return ret;
}

static int cnl_sdw_bra_pipe_cfg_pb(struct skl_sst *ctx,
					unsigned int mstr_num)
{
	struct bra_conf *bra_data = &ctx->bra_pipe_data[mstr_num];
	struct skl_pipe *host_cpr_pipe = NULL;
	struct skl_pipe_params host_cpr_params, link_cpr_params;
	struct skl_module_cfg *host_cpr_cfg = NULL, *link_cpr_cfg = NULL;
	struct skl_module *host_cpr_mod = NULL, *link_cpr_mod = NULL;
	int ret;
	struct skl_module_fmt *in_fmt, *out_fmt;
	u8 guid[16] = { 131, 12, 160, 155, 18, 202, 131,
			74, 148, 60, 31, 162, 232, 47, 157, 218 };

	host_cpr_cfg = kzalloc(sizeof(*host_cpr_cfg), GFP_KERNEL);
	if (!host_cpr_cfg) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg = kzalloc(sizeof(*link_cpr_cfg), GFP_KERNEL);
	if (!link_cpr_cfg) {
		ret = -ENOMEM;
		goto error;
	}

	host_cpr_mod = kzalloc(sizeof(*host_cpr_mod), GFP_KERNEL);
	if (!host_cpr_mod) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_mod = kzalloc(sizeof(*link_cpr_mod), GFP_KERNEL);
	if (!link_cpr_mod) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg->module = link_cpr_mod;
	host_cpr_cfg->module = host_cpr_mod;

	/*
	 * To get the pvt id, UUID of the module config is
	 * necessary. Hence hardocde this to the UUID fof copier
	 * module
	 */
	memcpy(host_cpr_cfg->guid, &guid, 16);
	memcpy(link_cpr_cfg->guid, &guid, 16);
	in_fmt = &host_cpr_cfg->module->formats[0].inputs[0].fmt;
	out_fmt = &host_cpr_cfg->module->formats[0].outputs[0].fmt;

	/* Playback pipeline */
	host_cpr_pipe = kzalloc(sizeof(struct skl_pipe), GFP_KERNEL);
	if (!host_cpr_pipe) {
		ret = -ENOMEM;
		goto error;
	}

	host_cpr_cfg->fmt_idx = 0;
	host_cpr_cfg->res_idx = 0;
	link_cpr_cfg->fmt_idx = 0;
	link_cpr_cfg->res_idx = 0;
	bra_data->pb_pipe = host_cpr_pipe;

	host_cpr_pipe->p_params = &host_cpr_params;
	host_cpr_cfg->pipe = host_cpr_pipe;

	host_cpr_pipe->ppl_id = 1;
	host_cpr_pipe->pipe_priority = 0;
	host_cpr_pipe->conn_type = 0;
	host_cpr_pipe->memory_pages = 2;

	ret = skl_create_pipeline(ctx, host_cpr_cfg->pipe);
	if (ret < 0)
		goto error;

	host_cpr_params.host_dma_id = (bra_data->pb_stream_tag - 1);
	host_cpr_params.link_dma_id = 0;
	host_cpr_params.ch = 1;
	host_cpr_params.s_freq = 96000;
	host_cpr_params.s_fmt = 32;
	host_cpr_params.linktype = 0;
	host_cpr_params.stream = 0;
	host_cpr_cfg->id.module_id = skl_get_module_id(ctx,
					(uuid_le *)host_cpr_cfg->guid);

	host_cpr_cfg->id.instance_id = 1;
	host_cpr_cfg->id.pvt_id = skl_get_pvt_id(ctx,
		(uuid_le *)host_cpr_cfg->guid, host_cpr_cfg->id.instance_id);
	if (host_cpr_cfg->id.pvt_id < 0)
		return -EINVAL;

	host_cpr_cfg->module->resources[0].cps = 100000;
	host_cpr_cfg->module->resources[0].is_pages = 0;
	host_cpr_cfg->module->resources[0].ibs = 384;
	host_cpr_cfg->module->resources[0].obs = 384;
	host_cpr_cfg->core_id = 0;
	host_cpr_cfg->module->max_input_pins = 1;
	host_cpr_cfg->module->max_output_pins = 1;
	host_cpr_cfg->module->loadable = 0;
	host_cpr_cfg->domain = 0;
	host_cpr_cfg->m_type = SKL_MODULE_TYPE_COPIER;
	host_cpr_cfg->dev_type = SKL_DEVICE_HDAHOST;
	host_cpr_cfg->hw_conn_type = SKL_CONN_SOURCE;
	host_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size = 0;
	host_cpr_cfg->module->resources[0].dma_buffer_size = 2;
	host_cpr_cfg->converter = 0;
	host_cpr_cfg->vbus_id = 0;
	host_cpr_cfg->sdw_agg_enable = 0;
	host_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size = 0;

	in_fmt->channels = 1;
	in_fmt->s_freq = 96000;
	in_fmt->bit_depth = 32;
	in_fmt->valid_bit_depth = 24;
	in_fmt->ch_cfg = 0;
	in_fmt->interleaving_style = 0;
	in_fmt->sample_type = 0;
	in_fmt->ch_map = 0xFFFFFFF1;

	out_fmt->channels = 1;
	out_fmt->s_freq = 96000;
	out_fmt->bit_depth = 32;
	out_fmt->valid_bit_depth = 24;
	out_fmt->ch_cfg = 0;
	out_fmt->interleaving_style = 0;
	out_fmt->sample_type = 0;
	out_fmt->ch_map = 0xFFFFFFF1;

	host_cpr_cfg->m_in_pin = kcalloc(host_cpr_cfg->module->max_input_pins,
					sizeof(*host_cpr_cfg->m_in_pin),
					GFP_KERNEL);
	if (!host_cpr_cfg->m_in_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	host_cpr_cfg->m_out_pin = kcalloc(host_cpr_cfg->module->max_output_pins,
					sizeof(*host_cpr_cfg->m_out_pin),
					GFP_KERNEL);
	if (!host_cpr_cfg->m_out_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	host_cpr_cfg->m_in_pin[0].id.module_id =
		host_cpr_cfg->id.module_id;
	host_cpr_cfg->m_in_pin[0].id.instance_id =
		host_cpr_cfg->id.instance_id;
	host_cpr_cfg->m_in_pin[0].in_use = false;
	host_cpr_cfg->m_in_pin[0].is_dynamic = true;
	host_cpr_cfg->m_in_pin[0].pin_state = SKL_PIN_UNBIND;

	host_cpr_cfg->m_out_pin[0].id.module_id =
		host_cpr_cfg->id.module_id;
	host_cpr_cfg->m_out_pin[0].id.instance_id =
		host_cpr_cfg->id.instance_id;
	host_cpr_cfg->m_out_pin[0].in_use = false;
	host_cpr_cfg->m_out_pin[0].is_dynamic = true;
	host_cpr_cfg->m_out_pin[0].pin_state = SKL_PIN_UNBIND;

	memcpy(link_cpr_cfg, host_cpr_cfg,
			sizeof(struct skl_module_cfg));
	memcpy(&link_cpr_params, &host_cpr_params,
			sizeof(struct skl_pipe_params));

	link_cpr_cfg->id.instance_id = 2;
	link_cpr_cfg->id.pvt_id = skl_get_pvt_id(ctx,
		(uuid_le *)link_cpr_cfg->guid, link_cpr_cfg->id.instance_id);
	if (link_cpr_cfg->id.pvt_id < 0)
		return -EINVAL;

	link_cpr_cfg->dev_type = SKL_DEVICE_SDW_PCM;
#if IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	link_cpr_cfg->sdw_stream_num = 0x3;
#else
	link_cpr_cfg->sdw_stream_num = 0x13;
#endif
	link_cpr_cfg->hw_conn_type = SKL_CONN_SOURCE;

	link_cpr_cfg->m_in_pin = kcalloc(link_cpr_cfg->module->max_input_pins,
					sizeof(*link_cpr_cfg->m_in_pin),
					GFP_KERNEL);
	if (!link_cpr_cfg->m_in_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	link_cpr_cfg->m_out_pin = kcalloc(link_cpr_cfg->module->max_output_pins,
					sizeof(*link_cpr_cfg->m_out_pin),
					GFP_KERNEL);
	if (!link_cpr_cfg->m_out_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	link_cpr_cfg->m_in_pin[0].id.module_id =
		link_cpr_cfg->id.module_id;
	link_cpr_cfg->m_in_pin[0].id.instance_id =
		link_cpr_cfg->id.instance_id;
	link_cpr_cfg->m_in_pin[0].in_use = false;
	link_cpr_cfg->m_in_pin[0].is_dynamic = true;
	link_cpr_cfg->m_in_pin[0].pin_state = SKL_PIN_UNBIND;

	link_cpr_cfg->m_out_pin[0].id.module_id =
		link_cpr_cfg->id.module_id;
	link_cpr_cfg->m_out_pin[0].id.instance_id =
		link_cpr_cfg->id.instance_id;
	link_cpr_cfg->m_out_pin[0].in_use = false;
	link_cpr_cfg->m_out_pin[0].is_dynamic = true;
	link_cpr_cfg->m_out_pin[0].pin_state = SKL_PIN_UNBIND;

	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size =
							(sizeof(u32) * 4);
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps =
				kzalloc((sizeof(u32) * 4), GFP_KERNEL);
	if (!link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[0] = 0x0;
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[1] = 0x1;
#if IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[2] = 0x1003;
#else
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[2] = 0x1013;
#endif
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[3] = 0x0;

	/* Init PB CPR1 module */
	ret = skl_init_module(ctx, host_cpr_cfg);
	if (ret < 0)
		goto error;

	/* Init PB CPR2 module */
	ret = skl_init_module(ctx, link_cpr_cfg);
	if (ret < 0)
		goto error;

	/* Bind PB CPR1 and CPR2 module */
	ret = skl_bind_modules(ctx, host_cpr_cfg, link_cpr_cfg);
	if (ret < 0)
		goto error;

error:
	/* Free up all memory allocated */
	kfree(host_cpr_cfg->m_in_pin);
	kfree(host_cpr_cfg->m_out_pin);
	kfree(link_cpr_cfg->m_in_pin);
	kfree(link_cpr_cfg->m_out_pin);
	kfree(link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps);
	kfree(host_cpr_cfg);
	kfree(link_cpr_cfg);
	kfree(host_cpr_mod);
	kfree(link_cpr_mod);

	return ret;
}

static int cnl_sdw_bra_pipe_cfg_cp(struct skl_sst *ctx,
					unsigned int mstr_num)
{
	struct bra_conf *bra_data = &ctx->bra_pipe_data[mstr_num];
	struct skl_pipe *link_cpr_pipe = NULL;
	struct skl_pipe_params link_cpr_params, host_cpr_params;
	struct skl_module *host_cpr_mod = NULL, *link_cpr_mod = NULL;
	struct skl_module_cfg *link_cpr_cfg = NULL, *host_cpr_cfg = NULL;
	int ret;
	struct skl_module_fmt *in_fmt, *out_fmt;
	u8 guid[16] = { 131, 12, 160, 155, 18, 202, 131,
			74, 148, 60, 31, 162, 232, 47, 157, 218 };

	link_cpr_cfg = kzalloc(sizeof(*link_cpr_cfg), GFP_KERNEL);
	if (!link_cpr_cfg) {
		ret = -ENOMEM;
		goto error;
	}

	host_cpr_cfg = kzalloc(sizeof(*host_cpr_cfg), GFP_KERNEL);
	if (!host_cpr_cfg) {
		ret = -ENOMEM;
		goto error;
	}

	host_cpr_mod = kzalloc(sizeof(*host_cpr_mod), GFP_KERNEL);
	if (!host_cpr_mod) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_mod = kzalloc(sizeof(*link_cpr_mod), GFP_KERNEL);
	if (!link_cpr_mod) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg->module = link_cpr_mod;
	host_cpr_cfg->module = host_cpr_mod;


	/*
	 * To get the pvt id, UUID of the module config is
	 * necessary. Hence hardocde this to the UUID fof copier
	 * module
	 */
	memcpy(host_cpr_cfg->guid, &guid, 16);
	memcpy(link_cpr_cfg->guid, &guid, 16);
	in_fmt = &link_cpr_cfg->module->formats[0].inputs[0].fmt;
	out_fmt = &link_cpr_cfg->module->formats[0].outputs[0].fmt;

	/* Capture Pipeline */
	link_cpr_pipe = kzalloc(sizeof(struct skl_pipe), GFP_KERNEL);
	if (!link_cpr_pipe) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg->fmt_idx = 0;
	link_cpr_cfg->res_idx = 0;
	host_cpr_cfg->fmt_idx = 0;
	host_cpr_cfg->res_idx = 0;
	bra_data->cp_pipe = link_cpr_pipe;
	link_cpr_pipe->p_params = &link_cpr_params;
	link_cpr_cfg->pipe = link_cpr_pipe;

	link_cpr_pipe->ppl_id = 2;
	link_cpr_pipe->pipe_priority = 0;
	link_cpr_pipe->conn_type = 0;
	link_cpr_pipe->memory_pages = 2;

	/* Create Capture Pipeline */
	ret = skl_create_pipeline(ctx, link_cpr_cfg->pipe);
	if (ret < 0)
		goto error;

	link_cpr_params.host_dma_id = 0;
	link_cpr_params.link_dma_id = 0;
	link_cpr_params.ch = 6;
	link_cpr_params.s_freq = 48000;
	link_cpr_params.s_fmt = 32;
	link_cpr_params.linktype = 0;
	link_cpr_params.stream = 0;
	link_cpr_cfg->id.module_id = skl_get_module_id(ctx,
					(uuid_le *)link_cpr_cfg->guid);

	link_cpr_cfg->id.instance_id = 3;
	link_cpr_cfg->id.pvt_id = skl_get_pvt_id(ctx,
		(uuid_le *)link_cpr_cfg->guid, link_cpr_cfg->id.instance_id);
	if (link_cpr_cfg->id.pvt_id < 0)
		return -EINVAL;

	link_cpr_cfg->module->resources[0].cps = 100000;
	link_cpr_cfg->module->resources[0].is_pages = 0;
	link_cpr_cfg->module->resources[0].ibs = 1152;
	link_cpr_cfg->module->resources[0].obs = 1152;
	link_cpr_cfg->core_id = 0;
	link_cpr_cfg->module->max_input_pins = 1;
	link_cpr_cfg->module->max_output_pins = 1;
	link_cpr_cfg->module->loadable = 0;
	link_cpr_cfg->domain = 0;
	link_cpr_cfg->m_type = SKL_MODULE_TYPE_COPIER;
	link_cpr_cfg->dev_type = SKL_DEVICE_SDW_PCM;
#if IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	link_cpr_cfg->sdw_stream_num = 0x4;
#else
	link_cpr_cfg->sdw_stream_num = 0x14;
#endif
	link_cpr_cfg->hw_conn_type = SKL_CONN_SINK;

	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size = 0;
	link_cpr_cfg->module->resources[0].dma_buffer_size = 2;
	link_cpr_cfg->converter = 0;
	link_cpr_cfg->vbus_id = 0;
	link_cpr_cfg->sdw_agg_enable = 0;
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size =
							(sizeof(u32) * 4);
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps =
				kzalloc((sizeof(u32) * 4), GFP_KERNEL);
	if (!link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps) {
		ret = -ENOMEM;
		goto error;
	}

	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[0] = 0x0;
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[1] = 0x1;
#if IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[2] = 0x1104;
#else
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[2] = 0x1114;
#endif
	link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps[3] = 0x1;

	in_fmt->channels = 6;
	in_fmt->s_freq = 48000;
	in_fmt->bit_depth = 32;
	in_fmt->valid_bit_depth = 24;
	in_fmt->ch_cfg = 8;
	in_fmt->interleaving_style = 0;
	in_fmt->sample_type = 0;
	in_fmt->ch_map = 0xFF657120;

	out_fmt->channels = 6;
	out_fmt->s_freq = 48000;
	out_fmt->bit_depth = 32;
	out_fmt->valid_bit_depth = 24;
	out_fmt->ch_cfg = 8;
	out_fmt->interleaving_style = 0;
	out_fmt->sample_type = 0;
	out_fmt->ch_map = 0xFF657120;

	link_cpr_cfg->m_in_pin = kcalloc(link_cpr_cfg->module->max_input_pins,
					sizeof(*link_cpr_cfg->m_in_pin),
					GFP_KERNEL);
	if (!link_cpr_cfg->m_in_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	link_cpr_cfg->m_out_pin = kcalloc(link_cpr_cfg->module->max_output_pins,
					sizeof(*link_cpr_cfg->m_out_pin),
					GFP_KERNEL);
	if (!link_cpr_cfg->m_out_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	link_cpr_cfg->m_in_pin[0].id.module_id =
		link_cpr_cfg->id.module_id;
	link_cpr_cfg->m_in_pin[0].id.instance_id =
		link_cpr_cfg->id.instance_id;
	link_cpr_cfg->m_in_pin[0].in_use = false;
	link_cpr_cfg->m_in_pin[0].is_dynamic = true;
	link_cpr_cfg->m_in_pin[0].pin_state = SKL_PIN_UNBIND;

	link_cpr_cfg->m_out_pin[0].id.module_id =
		link_cpr_cfg->id.module_id;
	link_cpr_cfg->m_out_pin[0].id.instance_id =
		link_cpr_cfg->id.instance_id;
	link_cpr_cfg->m_out_pin[0].in_use = false;
	link_cpr_cfg->m_out_pin[0].is_dynamic = true;
	link_cpr_cfg->m_out_pin[0].pin_state = SKL_PIN_UNBIND;

	memcpy(host_cpr_cfg, link_cpr_cfg,
			sizeof(struct skl_module_cfg));
	memcpy(&host_cpr_params, &link_cpr_params,
			sizeof(struct skl_pipe_params));

	host_cpr_cfg->id.instance_id = 4;
	host_cpr_cfg->id.pvt_id = skl_get_pvt_id(ctx,
		(uuid_le *)host_cpr_cfg->guid, host_cpr_cfg->id.instance_id);
	if (host_cpr_cfg->id.pvt_id < 0)
		return -EINVAL;

	host_cpr_cfg->dev_type = SKL_DEVICE_HDAHOST;
	host_cpr_cfg->hw_conn_type = SKL_CONN_SINK;
	link_cpr_params.host_dma_id = (bra_data->cp_stream_tag - 1);
	host_cpr_params.host_dma_id = (bra_data->cp_stream_tag - 1);
	host_cpr_cfg->formats_config[SKL_PARAM_INIT].caps_size = 0;
	host_cpr_cfg->m_in_pin = kcalloc(host_cpr_cfg->module->max_input_pins,
					sizeof(*host_cpr_cfg->m_in_pin),
					GFP_KERNEL);
	if (!host_cpr_cfg->m_in_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	host_cpr_cfg->m_out_pin = kcalloc(host_cpr_cfg->module->max_output_pins,
					sizeof(*host_cpr_cfg->m_out_pin),
					GFP_KERNEL);
	if (!host_cpr_cfg->m_out_pin) {
		ret =  -ENOMEM;
		goto error;
	}

	host_cpr_cfg->m_in_pin[0].id.module_id =
		host_cpr_cfg->id.module_id;
	host_cpr_cfg->m_in_pin[0].id.instance_id =
		host_cpr_cfg->id.instance_id;
	host_cpr_cfg->m_in_pin[0].in_use = false;
	host_cpr_cfg->m_in_pin[0].is_dynamic = true;
	host_cpr_cfg->m_in_pin[0].pin_state = SKL_PIN_UNBIND;

	host_cpr_cfg->m_out_pin[0].id.module_id =
		host_cpr_cfg->id.module_id;
	host_cpr_cfg->m_out_pin[0].id.instance_id =
		host_cpr_cfg->id.instance_id;
	host_cpr_cfg->m_out_pin[0].in_use = false;
	host_cpr_cfg->m_out_pin[0].is_dynamic = true;
	host_cpr_cfg->m_out_pin[0].pin_state = SKL_PIN_UNBIND;

	/* Init CP CPR1 module */
	ret = skl_init_module(ctx, link_cpr_cfg);
	if (ret < 0)
		goto error;

	/* Init CP CPR2 module */
	ret = skl_init_module(ctx, host_cpr_cfg);
	if (ret < 0)
		goto error;

	/* Bind CP CPR1 and CPR2 module */
	ret = skl_bind_modules(ctx, link_cpr_cfg, host_cpr_cfg);
	if (ret < 0)
		goto error;


error:
	/* Free up all memory allocated */
	kfree(link_cpr_cfg->formats_config[SKL_PARAM_INIT].caps);
	kfree(link_cpr_cfg->m_in_pin);
	kfree(link_cpr_cfg->m_out_pin);
	kfree(host_cpr_cfg->m_in_pin);
	kfree(host_cpr_cfg->m_out_pin);
	kfree(link_cpr_cfg);
	kfree(host_cpr_cfg);
	kfree(host_cpr_mod);
	kfree(link_cpr_mod);
	return ret;
}

static int cnl_sdw_bra_pipe_setup(struct skl_sst *ctx, bool enable,
						unsigned int mstr_num)
{
	struct bra_conf *bra_data = &ctx->bra_pipe_data[mstr_num];
	int ret;

	/*
	 * This function creates TX and TX pipelines for BRA transfers.
	 * TODO: Currently the pipelines are created manually. All the
	 * values needs to be received from XML based on the configuration
	 * used.
	 */

	if (enable) {

		/* Create playback pipeline */
		ret = cnl_sdw_bra_pipe_cfg_pb(ctx, mstr_num);
		if (ret < 0)
			goto error;

		/* Create capture pipeline */
		ret = cnl_sdw_bra_pipe_cfg_cp(ctx, mstr_num);
		if (ret < 0)
			goto error;
	} else {

		/* Delete playback pipeline */
		ret = skl_delete_pipe(ctx, bra_data->pb_pipe);
		if (ret < 0)
			goto error;

		/* Delete capture pipeline */
		ret = skl_delete_pipe(ctx, bra_data->cp_pipe);
		if (ret < 0)
			goto error;
	}

	if (enable)
		return 0;
error:
	/* Free up all memory allocated */
	kfree(bra_data->pb_pipe);
	kfree(bra_data->cp_pipe);

	return ret;
}

static int cnl_sdw_bra_dma_trigger(struct skl_sst *ctx, bool enable,
			unsigned int mstr_num)
{
	struct sst_dsp *dsp_ctx = ctx->dsp;
	struct bra_conf *bra_data = &ctx->bra_pipe_data[mstr_num];
	int ret;

	if (enable) {

		ret = dsp_ctx->dsp_ops.trigger(dsp_ctx->dev, true,
						bra_data->cp_stream_tag,
						SNDRV_PCM_STREAM_CAPTURE);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: RX DMA trigger failed: 0x%x\n", ret);
			goto bra_dma_failed;
		}

		ret = dsp_ctx->dsp_ops.trigger(dsp_ctx->dev, true,
						bra_data->pb_stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: TX DMA trigger failed: 0x%x\n", ret);
			goto bra_dma_failed;
		}

	} else {

		ret = dsp_ctx->dsp_ops.trigger(dsp_ctx->dev, false,
						bra_data->cp_stream_tag,
						SNDRV_PCM_STREAM_CAPTURE);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: RX DMA trigger stop failed: 0x%x\n", ret);
			goto bra_dma_failed;
		}
		ret = dsp_ctx->dsp_ops.trigger(dsp_ctx->dev, false,
						bra_data->pb_stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
		if (ret < 0) {
			dev_err(ctx->dev, "BRA: TX DMA trigger stop failed: 0x%x\n", ret);
			goto bra_dma_failed;
		}
	}

	if (enable)
		return 0;

bra_dma_failed:

	/* Free up resources */
	dsp_ctx->dsp_ops.cleanup(dsp_ctx->dev, &bra_data->pb_dmab,
						bra_data->pb_stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
	dsp_ctx->dsp_ops.cleanup(dsp_ctx->dev, &bra_data->cp_dmab,
						bra_data->cp_stream_tag,
						SNDRV_PCM_STREAM_CAPTURE);

	return ret;
}


static int cnl_sdw_bra_dma_setup(struct skl_sst *ctx, bool enable,
						struct bra_info *info)
{
	struct sst_dsp *dsp_ctx = ctx->dsp;
	struct bra_conf *bra_data = &ctx->bra_pipe_data[info->mstr_num];
	struct snd_dma_buffer *pb_dmab = &bra_data->pb_dmab;
	struct snd_dma_buffer *cp_dmab = &bra_data->cp_dmab;
	u32 pb_pages = 0, cp_pages = 0;
	int pb_block_size = info->tx_block_size;
	int cp_block_size = info->rx_block_size;
	int ret = 0;

	/*
	 * TODO: In future below approach can be replaced by component
	 * framework
	 */
	if (enable) {

		/*
		 * Take below number for BRA DMA format
		 * Format = (32 * 2 = 64) = 0x40 Size = 0x80
		 */

		/* Prepare TX Host DMA */
		bra_data->pb_stream_tag = dsp_ctx->dsp_ops.prepare(dsp_ctx->dev,
						0x40, pb_block_size,
						pb_dmab,
						SNDRV_PCM_STREAM_PLAYBACK);
		if (bra_data->pb_stream_tag <= 0) {
			dev_err(dsp_ctx->dev, "BRA: PB DMA prepare failed: 0x%x\n",
						bra_data->pb_stream_tag);
			ret = -EINVAL;
			goto bra_dma_failed;
		}

		pb_pages = (pb_block_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
		set_memory_uc((unsigned long) pb_dmab->area, pb_pages);
		memcpy(pb_dmab->area, info->tx_ptr, pb_block_size);

		/* Prepare RX Host DMA */
		bra_data->cp_stream_tag = dsp_ctx->dsp_ops.prepare(dsp_ctx->dev,
						0x40, cp_block_size,
						cp_dmab,
						SNDRV_PCM_STREAM_CAPTURE);
		if (bra_data->cp_stream_tag <= 0) {
			dev_err(dsp_ctx->dev, "BRA: CP DMA prepare failed: 0x%x\n",
						bra_data->cp_stream_tag);
			ret = -EINVAL;
			goto bra_dma_failed;
		}

		cp_pages = (cp_block_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
		set_memory_uc((unsigned long) cp_dmab->area, cp_pages);

	} else {

		ret = dsp_ctx->dsp_ops.cleanup(dsp_ctx->dev, &bra_data->pb_dmab,
						bra_data->pb_stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
		if (ret < 0)
			goto bra_dma_failed;

		ret = dsp_ctx->dsp_ops.cleanup(dsp_ctx->dev, &bra_data->cp_dmab,
						bra_data->cp_stream_tag,
						SNDRV_PCM_STREAM_CAPTURE);
		if (ret < 0)
			goto bra_dma_failed;

	}

bra_dma_failed:

	return ret;
}

static int cnl_sdw_bra_setup(void *context, bool enable,
			struct bra_info *info)
{
	struct skl_sst *ctx = context;
	int ret;

	if (enable) {

		/* Setup Host DMA */
		ret = cnl_sdw_bra_dma_setup(ctx, true, info);
		if (ret < 0)
			goto error;

		/* Create Pipeline */
		ret = cnl_sdw_bra_pipe_setup(ctx, true, info->mstr_num);
		if (ret < 0)
			goto error;

	} else {

		/* De-setup Host DMA */
		ret = cnl_sdw_bra_dma_setup(ctx, false, info);
		if (ret < 0)
			goto error;

		/* Delete Pipeline */
		ret = cnl_sdw_bra_pipe_setup(ctx, false, info->mstr_num);
		if (ret < 0)
			goto error;

	}

error:
	return ret;
}


static int cnl_sdw_bra_xfer(void *context, bool enable,
						struct bra_info *info)
{

	struct skl_sst *ctx = context;
	struct bra_conf *bra_data = &ctx->bra_pipe_data[info->mstr_num];
	struct snd_dma_buffer *cp_dmab = &bra_data->cp_dmab;
	int ret;

	if (enable) {

		/*
		 * TODO: Need to check on how to check on RX buffer
		 * completion. Approaches can be used:
		 * 1. Check any of LPIB, SPIB or DPIB register for
		 * xfer completion.
		 * 2. Add Interrupt of completion (IOC) for RX DMA buffer.
		 * This needs to adds changes in common infrastructure code
		 * only for BRA feature.
		 * Currenly we are just sleeping for 100 ms and copying
		 * data to appropriate RX buffer.
		 */

		/* Trigger Host DMA */
		ret = cnl_sdw_bra_dma_trigger(ctx, true, info->mstr_num);
		if (ret < 0)
			goto error;

		/* Trigger Pipeline */
		ret = cnl_sdw_bra_pipe_trigger(ctx, true, info->mstr_num);
		if (ret < 0)
			goto error;


		/* Sleep for 100 ms */
		msleep(100);

		/* TODO: Remove below hex dump print */
		print_hex_dump(KERN_DEBUG, "BRA CP DMA BUFFER DUMP RCVD:", DUMP_PREFIX_OFFSET, 8, 4,
			     cp_dmab->area, cp_dmab->bytes, false);

		/* Copy data in RX buffer */
		memcpy(info->rx_ptr, cp_dmab->area, info->rx_block_size);

	} else {

		/* Stop Host DMA */
		ret = cnl_sdw_bra_dma_trigger(ctx, false, info->mstr_num);
		if (ret < 0)
			goto error;

		/* Stop Pipeline */
		ret = cnl_sdw_bra_pipe_trigger(ctx, false, info->mstr_num);
		if (ret < 0)
			goto error;
	}

error:
	return ret;
}


struct cnl_bra_operation cnl_sdw_bra_ops = {
	.bra_platform_setup = cnl_sdw_bra_setup,
	.bra_platform_xfer = cnl_sdw_bra_xfer,
};


const struct skl_dsp_ops *skl_get_dsp_ops(int pci_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dsp_ops); i++) {
		if (dsp_ops[i].id == pci_id)
			return &dsp_ops[i];
	}

	return NULL;
}

int skl_init_dsp(struct skl *skl)
{
	void __iomem *mmio_base;
	struct hdac_ext_bus *ebus = &skl->ebus;
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct skl_dsp_loader_ops loader_ops;
	int irq = bus->irq;
	const struct skl_dsp_ops *ops;
	struct skl_dsp_cores *cores;
	int ret;

	/* enable ppcap interrupt */
	snd_hdac_ext_bus_ppcap_enable(&skl->ebus, true);
	snd_hdac_ext_bus_ppcap_int_enable(&skl->ebus, true);

	/* read the BAR of the ADSP MMIO */
	mmio_base = pci_ioremap_bar(skl->pci, 4);
	if (mmio_base == NULL) {
		dev_err(bus->dev, "ioremap error\n");
		return -ENXIO;
	}

	ops = skl_get_dsp_ops(skl->pci->device);
	if (!ops) {
		ret = -EIO;
		goto unmap_mmio;
	}

	loader_ops = ops->loader_ops();
	ret = ops->init(bus->dev, mmio_base, irq, skl->fw_name, loader_ops,
					&skl->skl_sst, &cnl_sdw_bra_ops);

	if (ret < 0)
		goto unmap_mmio;

	skl->skl_sst->dsp_ops = ops;
	cores = &skl->skl_sst->cores;
	cores->count = ops->num_cores;

	cores->state = kcalloc(cores->count, sizeof(*cores->state), GFP_KERNEL);
	if (!cores->state) {
		ret = -ENOMEM;
		goto unmap_mmio;
	}

	cores->usage_count = kcalloc(cores->count, sizeof(*cores->usage_count),
				     GFP_KERNEL);
	if (!cores->usage_count) {
		ret = -ENOMEM;
		goto free_core_state;
	}

	dev_dbg(bus->dev, "dsp registration status=%d\n", ret);

	INIT_LIST_HEAD(&skl->skl_sst->notify_kctls);

	/* Set DMA clock controls */
	ret = skl_dsp_set_dma_clk_controls(skl->skl_sst);
	if (ret < 0)
		return ret;
	return 0;

free_core_state:
	kfree(cores->state);

unmap_mmio:
	iounmap(mmio_base);

	return ret;
}

int skl_free_dsp(struct skl *skl)
{
	struct hdac_ext_bus *ebus = &skl->ebus;
	struct hdac_bus *bus = ebus_to_hbus(ebus);
	struct skl_sst *ctx = skl->skl_sst;
	struct skl_fw_property_info fw_property = skl->skl_sst->fw_property;
	struct skl_scheduler_config sch_config = fw_property.scheduler_config;

	/* disable  ppcap interrupt */
	snd_hdac_ext_bus_ppcap_int_enable(&skl->ebus, false);

	skl_module_sysfs_exit(skl->skl_sst);
	ctx->dsp_ops->cleanup(bus->dev, ctx);

	kfree(ctx->cores.state);
	kfree(ctx->cores.usage_count);

	if (ctx->dsp->addr.lpe)
		iounmap(ctx->dsp->addr.lpe);

	kfree(fw_property.dma_config);
	kfree(sch_config.sys_tick_cfg);

	return 0;
}

/*
 * In the case of "suspend_active" i.e, the Audio IP being active
 * during system suspend, immediately excecute any pending D0i3 work
 * before suspending. This is needed for the IP to work in low power
 * mode during system suspend. In the case of normal suspend, cancel
 * any pending D0i3 work.
 */
int skl_suspend_late_dsp(struct skl *skl)
{
	struct skl_sst *ctx = skl->skl_sst;
	struct delayed_work *dwork;

	if (!ctx)
		return 0;

	dwork = &ctx->d0i3.work;

	if (dwork->work.func) {
		if (skl->supend_active)
			flush_delayed_work(dwork);
		else
			cancel_delayed_work_sync(dwork);
	}

	return 0;
}

int skl_suspend_dsp(struct skl *skl)
{
	struct skl_sst *ctx = skl->skl_sst;
	int ret;

	/* if ppcap is not supported return 0 */
	if (!skl->ebus.bus.ppcap)
		return 0;

	ret = skl_dsp_sleep(ctx->dsp);
	if (ret < 0)
		return ret;

	/* disable ppcap interrupt */
	snd_hdac_ext_bus_ppcap_int_enable(&skl->ebus, false);
	snd_hdac_ext_bus_ppcap_enable(&skl->ebus, false);

	return 0;
}

int skl_resume_dsp(struct skl *skl)
{
	struct skl_sst *ctx = skl->skl_sst;
	int ret;

	/* if ppcap is not supported return 0 */
	if (!skl->ebus.bus.ppcap)
		return 0;

	/* enable ppcap interrupt */
	snd_hdac_ext_bus_ppcap_enable(&skl->ebus, true);
	snd_hdac_ext_bus_ppcap_int_enable(&skl->ebus, true);

	/* check if DSP 1st boot is done */
	if (skl->skl_sst->is_first_boot == true)
		return 0;

	ret = skl_dsp_wake(ctx->dsp);
	if (ret < 0)
		return ret;

	skl_dsp_enable_notification(skl->skl_sst, false);

	if (skl->cfg.astate_cfg != NULL) {
		skl_dsp_set_astate_cfg(skl->skl_sst, skl->cfg.astate_cfg->count,
					skl->cfg.astate_cfg);
	}

	/* Set DMA buffer configuration */
	if (skl->cfg.dmacfg.size)
		skl_ipc_set_dma_cfg(&skl->skl_sst->ipc, BXT_INSTANCE_ID,
			BXT_BASE_FW_MODULE_ID, (u32 *)(&skl->cfg.dmacfg));

	/* Set DMA clock controls */
	return skl_dsp_set_dma_clk_controls(skl->skl_sst);
}

enum skl_bitdepth skl_get_bit_depth(int params)
{
	switch (params) {
	case 8:
		return SKL_DEPTH_8BIT;

	case 16:
		return SKL_DEPTH_16BIT;

	case 24:
		return SKL_DEPTH_24BIT;

	case 32:
		return SKL_DEPTH_32BIT;

	default:
		return SKL_DEPTH_INVALID;

	}
}

/*
 * Each module in DSP expects a base module configuration, which consists of
 * PCM format information, which we calculate in driver and resource values
 * which are read from widget information passed through topology binary
 * This is send when we create a module with INIT_INSTANCE IPC msg
 */
static void skl_set_base_module_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_base_cfg *base_cfg)
{
	struct skl_module *module = mconfig->module;
	struct skl_module_res *res = &module->resources[mconfig->res_idx];
	struct skl_module_iface *fmt = &module->formats[mconfig->fmt_idx];
	struct skl_module_fmt *format = &fmt->inputs[0].fmt;

	base_cfg->audio_fmt.number_of_channels = format->channels;

	base_cfg->audio_fmt.s_freq = format->s_freq;
	base_cfg->audio_fmt.bit_depth = format->bit_depth;
	base_cfg->audio_fmt.valid_bit_depth = format->valid_bit_depth;
	base_cfg->audio_fmt.ch_cfg = format->ch_cfg;
	base_cfg->audio_fmt.sample_type = format->sample_type;

	dev_dbg(ctx->dev, "bit_depth=%x valid_bd=%x ch_config=%x sample_type:%x\n",
			format->bit_depth, format->valid_bit_depth,
			format->ch_cfg, format->sample_type);

	base_cfg->audio_fmt.channel_map = format->ch_map;

	base_cfg->audio_fmt.interleaving = format->interleaving_style;

	base_cfg->cps = res->cps;
	base_cfg->ibs = res->ibs;
	base_cfg->obs = res->obs;
	base_cfg->is_pages = res->is_pages;
}

/*
 * Copies copier capabilities into copier module and updates copier module
 * config size.
 */
static void skl_copy_copier_caps(struct skl_module_cfg *mconfig,
				struct skl_cpr_cfg *cpr_mconfig)
{
	if (mconfig->formats_config[SKL_PARAM_INIT].caps_size == 0)
		return;

	memcpy(cpr_mconfig->gtw_cfg.config_data,
			mconfig->formats_config[SKL_PARAM_INIT].caps,
			mconfig->formats_config[SKL_PARAM_INIT].caps_size);

	cpr_mconfig->gtw_cfg.config_length =
			(mconfig->formats_config[SKL_PARAM_INIT].caps_size) / 4;
}

#define SKL_NON_GATEWAY_CPR_NODE_ID 0xFFFFFFFF
/*
 * Calculate the gatewat settings required for copier module, type of
 * gateway and index of gateway to use
 */
static u32 skl_get_node_id(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig)
{
	union skl_connector_node_id node_id = {0};
	union skl_ssp_dma_node ssp_node  = {0};
	struct skl_pipe_params *params = mconfig->pipe->p_params;

	switch (mconfig->dev_type) {
	case SKL_DEVICE_BT:
		node_id.node.dma_type =
			(SKL_CONN_SOURCE == mconfig->hw_conn_type) ?
			SKL_DMA_I2S_LINK_OUTPUT_CLASS :
			SKL_DMA_I2S_LINK_INPUT_CLASS;
		node_id.node.vindex = params->host_dma_id +
					(mconfig->vbus_id << 3);
		break;

	case SKL_DEVICE_I2S:
		node_id.node.dma_type =
			(SKL_CONN_SOURCE == mconfig->hw_conn_type) ?
			SKL_DMA_I2S_LINK_OUTPUT_CLASS :
			SKL_DMA_I2S_LINK_INPUT_CLASS;
		ssp_node.dma_node.time_slot_index = mconfig->time_slot;
		ssp_node.dma_node.i2s_instance = mconfig->vbus_id;
		node_id.node.vindex = ssp_node.val;
		break;

	case SKL_DEVICE_DMIC:
		node_id.node.dma_type = SKL_DMA_DMIC_LINK_INPUT_CLASS;
		node_id.node.vindex = mconfig->vbus_id +
					 (mconfig->time_slot);
		break;

	case SKL_DEVICE_HDALINK:
		node_id.node.dma_type =
			(SKL_CONN_SOURCE == mconfig->hw_conn_type) ?
			SKL_DMA_HDA_LINK_OUTPUT_CLASS :
			SKL_DMA_HDA_LINK_INPUT_CLASS;
		node_id.node.vindex = params->link_dma_id;
		break;

	case SKL_DEVICE_HDAHOST:
		node_id.node.dma_type =
			(SKL_CONN_SOURCE == mconfig->hw_conn_type) ?
			SKL_DMA_HDA_HOST_OUTPUT_CLASS :
			SKL_DMA_HDA_HOST_INPUT_CLASS;
		node_id.node.vindex = params->host_dma_id;
		break;
	case SKL_DEVICE_SDW_PCM:
	case SKL_DEVICE_SDW_PDM:
		node_id.node.dma_type =
			(SKL_CONN_SOURCE == mconfig->hw_conn_type) ?
			SKL_DMA_SDW_LINK_OUTPUT_CLASS :
			SKL_DMA_SDW_LINK_INPUT_CLASS;
		if (mconfig->sdw_agg_enable)
			node_id.node.vindex = 0x50;
		else
			node_id.node.vindex = mconfig->sdw_stream_num;
		break;

	default:
		node_id.val = 0xFFFFFFFF;
		break;
	}

	return node_id.val;
}

static void skl_setup_cpr_gateway_cfg(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_cpr_cfg *cpr_mconfig)
{
	u32 dma_io_buf;
	struct skl_module_res *res;
	int res_idx = mconfig->res_idx;
	struct skl *skl = get_skl_ctx(ctx->dev);

	cpr_mconfig->gtw_cfg.node_id = skl_get_node_id(ctx, mconfig);

	if (cpr_mconfig->gtw_cfg.node_id == SKL_NON_GATEWAY_CPR_NODE_ID) {
		cpr_mconfig->cpr_feature_mask = 0;
		return;
	}

	if (skl->nr_modules) {
		res = &mconfig->module->resources[mconfig->res_idx];
		cpr_mconfig->gtw_cfg.dma_buffer_size = res->dma_buffer_size;
		goto skip_buf_size_calc;
	} else {
		res = &mconfig->module->resources[res_idx];
	}

	switch (mconfig->hw_conn_type) {
	case SKL_CONN_SOURCE:
		if (mconfig->dev_type == SKL_DEVICE_HDAHOST)
			dma_io_buf =  res->ibs;
		else
			dma_io_buf =  res->obs;
		break;

	case SKL_CONN_SINK:
		if (mconfig->dev_type == SKL_DEVICE_HDAHOST)
			dma_io_buf =  res->obs;
		else
			dma_io_buf =  res->ibs;
		break;

	default:
		dev_warn(ctx->dev, "wrong connection type: %d\n",
				mconfig->hw_conn_type);
		return;
	}

	cpr_mconfig->gtw_cfg.dma_buffer_size =
				mconfig->dma_buffer_size * dma_io_buf;

	/* fallback to 2ms default value */
	if (!cpr_mconfig->gtw_cfg.dma_buffer_size) {
		if (mconfig->hw_conn_type == SKL_CONN_SOURCE)
			cpr_mconfig->gtw_cfg.dma_buffer_size = 2 * res->obs;
		else
			cpr_mconfig->gtw_cfg.dma_buffer_size = 2 * res->ibs;
	}

skip_buf_size_calc:
	cpr_mconfig->cpr_feature_mask = 0;
	cpr_mconfig->gtw_cfg.config_length  = 0;

	skl_copy_copier_caps(mconfig, cpr_mconfig);
}

#define DMA_CONTROL_ID 5
#define DMA_I2S_BLOB_SIZE 21

int skl_dsp_set_dma_control(struct skl_sst *ctx, u32 *caps,
				u32 caps_size, u32 node_id)
{
	struct skl_dma_control *dma_ctrl;
	struct skl_ipc_large_config_msg msg = {0};
	int err = 0;


	/*
	 * if blob size zero, then return
	 */
	if (caps_size == 0)
		return 0;

	msg.large_param_id = DMA_CONTROL_ID;
	msg.param_data_size = sizeof(struct skl_dma_control) + caps_size;

	dma_ctrl = kzalloc(msg.param_data_size, GFP_KERNEL);
	if (dma_ctrl == NULL)
		return -ENOMEM;

	dma_ctrl->node_id = node_id;

	/*
	 * NHLT blob may contain additional configs along with i2s blob.
	 * firmware expects only the I2S blob size as the config_length. So fix to i2s
	 * blob size.
	 *
	 * size in dwords.
	 */
	dma_ctrl->config_length = DMA_I2S_BLOB_SIZE;

	memcpy(dma_ctrl->config_data, caps, caps_size);

	err = skl_ipc_set_large_config(&ctx->ipc, &msg, (u32 *)dma_ctrl);

	kfree(dma_ctrl);

	return err;
}

static u32 skl_prepare_i2s_node_id(u32 instance, u8 dev_type,
				u32 dir, u32 time_slot)
{
	union skl_connector_node_id node_id = {0};
	union skl_ssp_dma_node ssp_node  = {0};

	node_id.node.dma_type = (dir == SNDRV_PCM_STREAM_PLAYBACK) ?
					SKL_DMA_I2S_LINK_OUTPUT_CLASS :
					SKL_DMA_I2S_LINK_INPUT_CLASS;
	ssp_node.dma_node.time_slot_index = time_slot;
	ssp_node.dma_node.i2s_instance = instance;
	node_id.node.vindex = ssp_node.val;

	return node_id.val;
}

int skl_dsp_set_dma_clk_controls(struct skl_sst *ctx)
{
	struct nhlt_specific_cfg *cfg = NULL;
	struct skl *skl = get_skl_ctx(ctx->dev);
	struct skl_dmactrl_config *dmactrl_cfg = &skl->cfg.dmactrl_cfg;
	struct skl_dmctrl_hdr *hdr;
	u8 *dma_ctrl_config;
	void *i2s_config = NULL;
	u32 i2s_config_size, node_id;
	int i, ret = 0;

	if (!skl->cfg.dmactrl_cfg.size)
		return 0;

	for (i = 0; i < SKL_MAX_DMACTRL_CFG; i++) {
		hdr = &dmactrl_cfg->hdr[i];

		/* get nhlt specific config info */
		cfg = skl_get_nhlt_specific_cfg(skl, hdr->vbus_id,
					NHLT_LINK_SSP, hdr->fmt,
					hdr->ch, hdr->freq,
					hdr->direction, NHLT_DEVICE_I2S);

		if (cfg && hdr->data_size) {
			print_hex_dump(KERN_DEBUG, "NHLT blob Info:",
					DUMP_PREFIX_OFFSET, 8, 4,
					cfg->caps, cfg->size, false);

			i2s_config_size = cfg->size + hdr->data_size;
			i2s_config = kzalloc(i2s_config_size, GFP_KERNEL);
			if (!i2s_config)
				return -ENOMEM;

			/* copy blob */
			memcpy(i2s_config, cfg->caps, cfg->size);

			/* copy additional dma controls informatioin */
			dma_ctrl_config = (u8 *)i2s_config + cfg->size;
			memcpy(dma_ctrl_config, hdr->data, hdr->data_size);

			print_hex_dump(KERN_DEBUG, "Blob + DMA Control Info:",
					DUMP_PREFIX_OFFSET, 8, 4,
					i2s_config, i2s_config_size, false);

			/* get node id */
			node_id = skl_prepare_i2s_node_id(hdr->vbus_id,
							SKL_DEVICE_I2S,
							hdr->direction,
							hdr->tdm_slot);

			ret = skl_dsp_set_dma_control(ctx, (u32 *)i2s_config,
							i2s_config_size, node_id);

			kfree(i2s_config);

			if (ret < 0)
				return ret;

		} else {
			dev_err(ctx->dev, "Failed to get NHLT config: vbusi_id=%d ch=%d fmt=%d s_rate=%d\n",
				hdr->vbus_id, hdr->ch, hdr->fmt, hdr->freq);
			return -EIO;
		}
	}

	return 0;
}

static void skl_setup_out_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_audio_data_format *out_fmt)
{
	struct skl_module *module = mconfig->module;
	struct skl_module_iface *fmt = &module->formats[mconfig->fmt_idx];
	struct skl_module_fmt *format = &fmt->outputs[0].fmt;

	out_fmt->number_of_channels = (u8)format->channels;
	out_fmt->s_freq = format->s_freq;
	out_fmt->bit_depth = format->bit_depth;
	out_fmt->valid_bit_depth = format->valid_bit_depth;
	out_fmt->ch_cfg = format->ch_cfg;

	out_fmt->channel_map = format->ch_map;
	out_fmt->interleaving = format->interleaving_style;
	out_fmt->sample_type = format->sample_type;

	dev_dbg(ctx->dev, "copier out format chan=%d fre=%d bitdepth=%d\n",
		out_fmt->number_of_channels, format->s_freq, format->bit_depth);
}

static int skl_set_gain_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_gain_module_config *gain_mconfig)
{
	struct skl_gain_data *gain_fmt = mconfig->gain_data;

	skl_set_base_module_format(ctx, mconfig,
			(struct skl_base_cfg *)gain_mconfig);
	gain_mconfig->gain_cfg.channel_id = SKL_ENABLE_ALL_CHANNELS;
	gain_mconfig->gain_cfg.target_volume = gain_fmt->volume[0];
	gain_mconfig->gain_cfg.ramp_type = gain_fmt->ramp_type;
	gain_mconfig->gain_cfg.ramp_duration = gain_fmt->ramp_duration;

	return 0;
}

/*
 * DSP needs SRC module for frequency conversion, SRC takes base module
 * configuration and the target frequency as extra parameter passed as src
 * config
 */
static void skl_set_src_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_src_module_cfg *src_mconfig)
{
	struct skl_module *module = mconfig->module;
	struct skl_module_iface *iface = &module->formats[mconfig->fmt_idx];
	struct skl_module_fmt *fmt = &iface->outputs[0].fmt;

	skl_set_base_module_format(ctx, mconfig,
		(struct skl_base_cfg *)src_mconfig);

	src_mconfig->src_cfg = fmt->s_freq;

	if (mconfig->m_type == SKL_MODULE_TYPE_ASRC) {
		if (mconfig->pipe->p_params->stream ==
				SNDRV_PCM_STREAM_PLAYBACK)
			src_mconfig->mode = ASRC_MODE_DOWNLINK;
		else
			src_mconfig->mode = ASRC_MODE_UPLINK;
	}
}

/*
 * DSP needs updown module to do channel conversion. updown module take base
 * module configuration and channel configuration
 * It also take coefficients and now we have defaults applied here
 */
static void skl_set_updown_mixer_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_up_down_mixer_cfg *mixer_mconfig)
{
	struct skl_module *module = mconfig->module;
	struct skl_module_iface *iface = &module->formats[mconfig->fmt_idx];
	struct skl_module_fmt *fmt = &iface->outputs[0].fmt;
	int i = 0;

	skl_set_base_module_format(ctx,	mconfig,
		(struct skl_base_cfg *)mixer_mconfig);
	mixer_mconfig->out_ch_cfg = fmt->ch_cfg;

	/* Select F/W default coefficient */
	mixer_mconfig->coeff_sel = 0x0;
	mixer_mconfig->ch_map = fmt->ch_map;

	/* User coeff, don't care since we are selecting F/W defaults */
	for (i = 0; i < UP_DOWN_MIXER_MAX_COEFF; i++)
		mixer_mconfig->coeff[i] = 0x0;
}

/*
 * 'copier' is DSP internal module which copies data from Host DMA (HDA host
 * dma) or link (hda link, SSP, PDM)
 * Here we calculate the copier module parameters, like PCM format, output
 * format, gateway settings
 * copier_module_config is sent as input buffer with INIT_INSTANCE IPC msg
 */
static void skl_set_copier_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_cpr_cfg *cpr_mconfig)
{
	struct skl_audio_data_format *out_fmt = &cpr_mconfig->out_fmt;
	struct skl_base_cfg *base_cfg = (struct skl_base_cfg *)cpr_mconfig;

	skl_set_base_module_format(ctx, mconfig, base_cfg);

	skl_setup_out_format(ctx, mconfig, out_fmt);
	skl_setup_cpr_gateway_cfg(ctx, mconfig, cpr_mconfig);
}

static void skl_setup_probe_gateway_cfg(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_probe_cfg *probe_cfg)
{
	union skl_connector_node_id node_id = {0};
	struct skl_module_res *res;
	struct skl_probe_config *pconfig = &ctx->probe_config;

	res = &mconfig->module->resources[mconfig->res_idx];

	pconfig->edma_buffsize = res->dma_buffer_size;

	node_id.node.dma_type = pconfig->edma_type;
	node_id.node.vindex = pconfig->edma_id;
	probe_cfg->prb_cfg.dma_buffer_size = pconfig->edma_buffsize;

	memcpy(&(probe_cfg->prb_cfg.node_id), &node_id, sizeof(u32));
}

static void skl_set_probe_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_probe_cfg *probe_mconfig)
{
	struct skl_base_cfg *base_cfg = (struct skl_base_cfg *)probe_mconfig;

	skl_set_base_module_format(ctx, mconfig, base_cfg);
	skl_setup_probe_gateway_cfg(ctx, mconfig, probe_mconfig);
}

/*
 * Algo module are DSP pre processing modules. Algo module take base module
 * configuration and params
 */

static void skl_set_algo_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_algo_cfg *algo_mcfg)
{
	struct skl_base_cfg *base_cfg = (struct skl_base_cfg *)algo_mcfg;

	skl_set_base_module_format(ctx, mconfig, base_cfg);

	if (mconfig->formats_config[SKL_PARAM_INIT].caps_size == 0)
		return;

	memcpy(algo_mcfg->params,
			mconfig->formats_config[SKL_PARAM_INIT].caps,
			mconfig->formats_config[SKL_PARAM_INIT].caps_size);

}

/*
 * Mic select module allows selecting one or many input channels, thus
 * acting as a demux.
 *
 * Mic select module take base module configuration and out-format
 * configuration
 */
static void skl_set_base_outfmt_format(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig,
			struct skl_base_outfmt_cfg *base_outfmt_mcfg)
{
	struct skl_audio_data_format *out_fmt = &base_outfmt_mcfg->out_fmt;
	struct skl_base_cfg *base_cfg =
				(struct skl_base_cfg *)base_outfmt_mcfg;

	skl_set_base_module_format(ctx, mconfig, base_cfg);
	skl_setup_out_format(ctx, mconfig, out_fmt);
}

static u16 skl_get_module_param_size(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig)
{
	u16 param_size;
	struct skl_module_iface *m_intf;

	switch (mconfig->m_type) {
	case SKL_MODULE_TYPE_COPIER:
		param_size = sizeof(struct skl_cpr_cfg);
		param_size += mconfig->formats_config[SKL_PARAM_INIT].caps_size;
		return param_size;

	case SKL_MODULE_TYPE_PROBE:
		return sizeof(struct skl_probe_cfg);

	case SKL_MODULE_TYPE_SRCINT:
	case SKL_MODULE_TYPE_ASRC:
		return sizeof(struct skl_src_module_cfg);

	case SKL_MODULE_TYPE_UPDWMIX:
		return sizeof(struct skl_up_down_mixer_cfg);

	case SKL_MODULE_TYPE_ALGO:
		param_size = sizeof(struct skl_base_cfg);
		param_size += mconfig->formats_config[SKL_PARAM_INIT].caps_size;
		return param_size;

	case SKL_MODULE_TYPE_BASE_OUTFMT:
	case SKL_MODULE_TYPE_MIC_SELECT:
	case SKL_MODULE_TYPE_KPB:
		return sizeof(struct skl_base_outfmt_cfg);

	case SKL_MODULE_TYPE_GAIN:
		m_intf = &mconfig->module->formats[mconfig->fmt_idx];
		param_size = sizeof(struct skl_base_cfg);
		param_size += sizeof(struct skl_gain_config)
			* m_intf->outputs[0].fmt.channels;
		return param_size;

	default:
		/*
		 * return only base cfg when no specific module type is
		 * specified
		 */
		return sizeof(struct skl_base_cfg);
	}

	return 0;
}

/*
 * DSP firmware supports various modules like copier, SRC, updown etc.
 * These modules required various parameters to be calculated and sent for
 * the module initialization to DSP. By default a generic module needs only
 * base module format configuration
 */

static int skl_set_module_format(struct skl_sst *ctx,
			struct skl_module_cfg *module_config,
			u16 *module_config_size,
			void **param_data)
{
	u16 param_size;

	param_size  = skl_get_module_param_size(ctx, module_config);

	*param_data = kzalloc(param_size, GFP_KERNEL);
	if (NULL == *param_data)
		return -ENOMEM;

	*module_config_size = param_size;

	switch (module_config->m_type) {
	case SKL_MODULE_TYPE_COPIER:
		skl_set_copier_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_PROBE:
		skl_set_probe_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_SRCINT:
	case SKL_MODULE_TYPE_ASRC:
		skl_set_src_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_UPDWMIX:
		skl_set_updown_mixer_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_ALGO:
		skl_set_algo_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_BASE_OUTFMT:
	case SKL_MODULE_TYPE_MIC_SELECT:
	case SKL_MODULE_TYPE_KPB:
		skl_set_base_outfmt_format(ctx, module_config, *param_data);
		break;

	case SKL_MODULE_TYPE_GAIN:
		skl_set_gain_format(ctx, module_config, *param_data);
		break;

	default:
		skl_set_base_module_format(ctx, module_config, *param_data);
		break;

	}

	dev_dbg(ctx->dev, "Module type=%d config size: %d bytes\n",
			module_config->id.module_id, param_size);
	print_hex_dump_debug("Module params:", DUMP_PREFIX_OFFSET, 8, 4,
			*param_data, param_size, false);
	return 0;
}

static int skl_get_queue_index(struct skl_module_pin *mpin,
				struct skl_module_inst_id id, int max)
{
	int i;

	for (i = 0; i < max; i++)  {
		if (mpin[i].id.module_id == id.module_id &&
			mpin[i].id.instance_id == id.instance_id)
			return i;
	}

	return -EINVAL;
}

/*
 * Allocates queue for each module.
 * if dynamic, the pin_index is allocated 0 to max_pin.
 * In static, the pin_index is fixed based on module_id and instance id
 */
static int skl_alloc_queue(struct skl_module_pin *mpin,
			struct skl_module_cfg *tgt_cfg, int max)
{
	int i;
	struct skl_module_inst_id id = tgt_cfg->id;
	/*
	 * if pin in dynamic, find first free pin
	 * otherwise find match module and instance id pin as topology will
	 * ensure a unique pin is assigned to this so no need to
	 * allocate/free
	 */
	for (i = 0; i < max; i++)  {
		if (mpin[i].is_dynamic) {
			if (!mpin[i].in_use &&
				mpin[i].pin_state == SKL_PIN_UNBIND) {

				mpin[i].in_use = true;
				mpin[i].id.module_id = id.module_id;
				mpin[i].id.instance_id = id.instance_id;
				mpin[i].id.pvt_id = id.pvt_id;
				mpin[i].tgt_mcfg = tgt_cfg;
				return i;
			}
		} else {
			if (mpin[i].id.module_id == id.module_id &&
				mpin[i].id.instance_id == id.instance_id &&
				mpin[i].pin_state == SKL_PIN_UNBIND) {

				mpin[i].tgt_mcfg = tgt_cfg;
				return i;
			}
		}
	}

	return -EINVAL;
}

static void skl_free_queue(struct skl_module_pin *mpin, int q_index)
{
	if (mpin[q_index].is_dynamic) {
		mpin[q_index].in_use = false;
		mpin[q_index].id.module_id = 0;
		mpin[q_index].id.instance_id = 0;
		mpin[q_index].id.pvt_id = 0;
	}
	mpin[q_index].pin_state = SKL_PIN_UNBIND;
	mpin[q_index].tgt_mcfg = NULL;
}

/* Module state will be set to unint, if all the out pin state is UNBIND */

static void skl_clear_module_state(struct skl_module_pin *mpin, int max,
						struct skl_module_cfg *mcfg)
{
	int i;
	bool found = false;

	for (i = 0; i < max; i++)  {
		if (mpin[i].pin_state == SKL_PIN_UNBIND)
			continue;
		found = true;
		break;
	}

	if (!found)
		mcfg->m_state = SKL_MODULE_INIT_DONE;
	return;
}

/*
 * A module needs to be instanataited in DSP. A mdoule is present in a
 * collection of module referred as a PIPE.
 * We first calculate the module format, based on module type and then
 * invoke the DSP by sending IPC INIT_INSTANCE using ipc helper
 */
int skl_init_module(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig)
{
	u16 module_config_size = 0;
	void *param_data = NULL;
	int ret;
	struct skl_ipc_init_instance_msg msg;

	dev_dbg(ctx->dev, "%s: module_id = %d instance=%d\n", __func__,
		 mconfig->id.module_id, mconfig->id.pvt_id);

	if (mconfig->pipe->state != SKL_PIPE_CREATED) {
		dev_err(ctx->dev, "Pipe not created state= %d pipe_id= %d\n",
				 mconfig->pipe->state, mconfig->pipe->ppl_id);
		return -EIO;
	}

	ret = skl_set_module_format(ctx, mconfig,
			&module_config_size, &param_data);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to set module format ret=%d\n", ret);
		return ret;
	}

	msg.module_id = mconfig->id.module_id;
	msg.instance_id = mconfig->id.pvt_id;
	msg.ppl_instance_id = mconfig->pipe->ppl_id;
	msg.param_data_size = module_config_size;
	msg.core_id = mconfig->core_id;
	msg.domain = mconfig->domain;

	ret = skl_ipc_init_instance(&ctx->ipc, &msg, param_data);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to init instance ret=%d\n", ret);
		kfree(param_data);
		return ret;
	}
	mconfig->m_state = SKL_MODULE_INIT_DONE;
	kfree(param_data);
	return ret;
}

int skl_init_probe_module(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig)
{
	u16 module_config_size = 0;
	void *param_data = NULL;
	int ret;
	struct skl_ipc_init_instance_msg msg;

	dev_dbg(ctx->dev, "%s: module_id = %d instance=%d\n", __func__,
		 mconfig->id.module_id, mconfig->id.instance_id);


	ret = skl_set_module_format(ctx, mconfig,
			&module_config_size, &param_data);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to set module format ret=%d\n", ret);
		return ret;
	}

	msg.module_id = mconfig->id.module_id;
	msg.instance_id = mconfig->id.instance_id;
	msg.ppl_instance_id = -1;
	msg.param_data_size = module_config_size;
	msg.core_id = mconfig->core_id;
	msg.domain = mconfig->domain;

	ret = skl_ipc_init_instance(&ctx->ipc, &msg, param_data);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to init instance ret=%d\n", ret);
		kfree(param_data);
		return ret;
	}
	mconfig->m_state = SKL_MODULE_INIT_DONE;
	kfree(param_data);
	return ret;
}

int skl_uninit_probe_module(struct skl_sst *ctx,
			struct skl_module_cfg *mconfig)
{
	u16 module_config_size = 0;
	int ret;
	struct skl_ipc_init_instance_msg msg;

	dev_dbg(ctx->dev, "%s: module_id = %d instance=%d\n", __func__,
		 mconfig->id.module_id, mconfig->id.instance_id);

	msg.module_id = mconfig->id.module_id;
	msg.instance_id = mconfig->id.instance_id;
	msg.ppl_instance_id = -1;
	msg.param_data_size = module_config_size;
	msg.core_id = mconfig->core_id;
	msg.domain = mconfig->domain;

	ret = skl_ipc_delete_instance(&ctx->ipc, &msg);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to delete instance ret=%d\n", ret);
		return ret;
	}
	mconfig->m_state = SKL_MODULE_UNINIT;

	return ret;
}

static void skl_dump_bind_info(struct skl_sst *ctx, struct skl_module_cfg
	*src_module, struct skl_module_cfg *dst_module)
{
	dev_dbg(ctx->dev, "%s: src module_id = %d  src_instance=%d\n",
		__func__, src_module->id.module_id, src_module->id.pvt_id);
	dev_dbg(ctx->dev, "%s: dst_module=%d dst_instance=%d\n", __func__,
		 dst_module->id.module_id, dst_module->id.pvt_id);

	dev_dbg(ctx->dev, "src_module state = %d dst module state = %d\n",
		src_module->m_state, dst_module->m_state);
}

int skl_probe_point_disconnect_ext(struct skl_sst *ctx,
				struct snd_soc_dapm_widget *w)
{
	struct skl_ipc_large_config_msg msg;
	struct skl_probe_config *pconfig = &ctx->probe_config;
	struct skl_module_cfg *mcfg;
	u32 probe_point[NO_OF_EXTRACTOR] = {0};
	int store_prb_pt_index[NO_OF_EXTRACTOR] = {0};
	int n = 0, i;
	int ret = 0;
	int no_of_extractor = pconfig->no_extractor;

	dev_dbg(ctx->dev, "Disconnecting extractor probe points\n");
	mcfg = w->priv;
	msg.module_id = mcfg->id.module_id;
	msg.instance_id = mcfg->id.instance_id;
	msg.large_param_id = SKL_PROBE_DISCONNECT;

	for (i = 0; i < no_of_extractor; i++) {
		if (pconfig->eprobe[i].state == SKL_PROBE_STATE_EXT_CONNECTED) {
			probe_point[n] = pconfig->eprobe[i].probe_point_id;
			store_prb_pt_index[i] = 1;
			n++;
		}
	}
	if (n == 0)
		return ret;

	msg.param_data_size = n * sizeof(u32);
	dev_dbg(ctx->dev, "setting module params size=%d\n",
					msg.param_data_size);
	ret = skl_ipc_set_large_config(&ctx->ipc, &msg, probe_point);
	if (ret < 0)
		return -EINVAL;

	for (i = 0; i < pconfig->no_extractor; i++) {
		if (store_prb_pt_index[i]) {
			pconfig->eprobe[i].state = SKL_PROBE_STATE_EXT_NONE;
			dev_dbg(ctx->dev, "eprobe[%d].state %d\n",
					i, pconfig->eprobe[i].state);
		}
	}

	return ret;
}

int skl_probe_point_disconnect_inj(struct skl_sst *ctx,
				struct snd_soc_dapm_widget *w, int index)
{
	struct skl_ipc_large_config_msg msg;
	struct skl_probe_config *pconfig = &ctx->probe_config;
	struct skl_module_cfg *mcfg;
	u32 probe_point = 0;
	int ret = 0;

	if (pconfig->iprobe[index].state == SKL_PROBE_STATE_INJ_CONNECTED) {
		dev_dbg(ctx->dev, "Disconnecting injector probe point\n");
		mcfg = w->priv;
		msg.module_id = mcfg->id.module_id;
		msg.instance_id = mcfg->id.instance_id;
		msg.large_param_id = SKL_PROBE_DISCONNECT;
		probe_point = pconfig->iprobe[index].probe_point_id;
		msg.param_data_size = sizeof(u32);

		dev_dbg(ctx->dev, "setting module params size=%d\n",
						msg.param_data_size);
		ret = skl_ipc_set_large_config(&ctx->ipc, &msg, &probe_point);
		if (ret < 0)
			return -EINVAL;

		pconfig->iprobe[index].state = SKL_PROBE_STATE_INJ_DISCONNECTED;
		dev_dbg(ctx->dev, "iprobe[%d].state %d\n",
				index, pconfig->iprobe[index].state);
	}

	return ret;

}
/*
 * On module freeup, we need to unbind the module with modules
 * it is already bind.
 * Find the pin allocated and unbind then using bind_unbind IPC
 */
int skl_unbind_modules(struct skl_sst *ctx,
			struct skl_module_cfg *src_mcfg,
			struct skl_module_cfg *dst_mcfg)
{
	int ret;
	struct skl_ipc_bind_unbind_msg msg;
	struct skl_module_inst_id src_id = src_mcfg->id;
	struct skl_module_inst_id dst_id = dst_mcfg->id;
	int in_max = dst_mcfg->module->max_input_pins;
	int out_max = src_mcfg->module->max_output_pins;
	int src_index, dst_index, src_pin_state, dst_pin_state;

	skl_dump_bind_info(ctx, src_mcfg, dst_mcfg);

	/* get src queue index */
	src_index = skl_get_queue_index(src_mcfg->m_out_pin, dst_id, out_max);
	if (src_index < 0)
		return 0;

	msg.src_queue = src_index;

	/* get dst queue index */
	dst_index  = skl_get_queue_index(dst_mcfg->m_in_pin, src_id, in_max);
	if (dst_index < 0)
		return 0;

	msg.dst_queue = dst_index;

	src_pin_state = src_mcfg->m_out_pin[src_index].pin_state;
	dst_pin_state = dst_mcfg->m_in_pin[dst_index].pin_state;

	if (src_pin_state != SKL_PIN_BIND_DONE ||
		dst_pin_state != SKL_PIN_BIND_DONE)
		return 0;

	msg.module_id = src_mcfg->id.module_id;
	msg.instance_id = src_mcfg->id.pvt_id;
	msg.dst_module_id = dst_mcfg->id.module_id;
	msg.dst_instance_id = dst_mcfg->id.pvt_id;
	msg.bind = false;

	ret = skl_ipc_bind_unbind(&ctx->ipc, &msg);
	if (!ret) {
		/* free queue only if unbind is success */
		skl_free_queue(src_mcfg->m_out_pin, src_index);
		skl_free_queue(dst_mcfg->m_in_pin, dst_index);

		/*
		 * check only if src module bind state, bind is
		 * always from src -> sink
		 */
		skl_clear_module_state(src_mcfg->m_out_pin, out_max, src_mcfg);
	}

	return ret;
}

static void fill_pin_params(struct skl_audio_data_format *pin_fmt,
				struct skl_module_fmt *format)
{
	pin_fmt->number_of_channels = format->channels;
	pin_fmt->s_freq = format->s_freq;
	pin_fmt->bit_depth = format->bit_depth;
	pin_fmt->valid_bit_depth = format->valid_bit_depth;
	pin_fmt->ch_cfg = format->ch_cfg;
	pin_fmt->sample_type = format->sample_type;
	pin_fmt->channel_map = format->ch_map;
	pin_fmt->interleaving = format->interleaving_style;
}

#define CPR_SINK_FMT_PARAM_ID 2

static struct
skl_module_fmt *skl_get_pin_format(struct skl_module_cfg *mconfig,
				   u8 pin_direction, u8 pin_idx)
{
	struct skl_module *module = mconfig->module;
	int fmt_idx = mconfig->fmt_idx;
	struct skl_module_iface *intf;
	struct skl_module_fmt *pin_fmt;

	intf = &module->formats[fmt_idx];

	if (pin_direction == SKL_INPUT_PIN)
		pin_fmt = &intf->inputs[pin_idx].fmt;
	else
		pin_fmt = &intf->outputs[pin_idx].fmt;

	return pin_fmt;
}

/*
 * This function checks for source module and destination module format
 * mismatch
 */
static void skl_module_format_mismatch_detection(struct skl_sst *ctx,
					struct skl_module_cfg *src_mcfg,
					struct skl_module_cfg *dst_mcfg,
					int src_index, int dst_index)
{
	struct skl_module_fmt *src_fmt, *dst_fmt;

	src_fmt = skl_get_pin_format(src_mcfg, SKL_OUTPUT_PIN, src_index);
	dst_fmt = skl_get_pin_format(dst_mcfg, SKL_INPUT_PIN, dst_index);

	if(memcmp(src_fmt, dst_fmt, sizeof(*src_fmt))) {
		dev_warn(ctx->dev, "#### src and dst format mismatch: ####\n");
		dev_warn(ctx->dev, "pipe=%d src module_id=%d src instance_id=%d\n",
					src_mcfg->pipe->ppl_id,
					src_mcfg->id.module_id,
					src_mcfg->id.pvt_id);

		dev_warn(ctx->dev, "pipe=%d dst module_id=%d dst instance_id=%d\n",
					dst_mcfg->pipe->ppl_id,
					dst_mcfg->id.module_id,
					dst_mcfg->id.pvt_id);

		dev_warn(ctx->dev, "channels: src=%d dst=%d\n",
				src_fmt->channels, dst_fmt->channels);
		dev_warn(ctx->dev, "s_freq: src=%d dst=%d\n",
				src_fmt->s_freq, dst_fmt->s_freq);
		dev_warn(ctx->dev, "bit_depth: src=%d dst=%d\n",
				src_fmt->bit_depth, dst_fmt->bit_depth);
		dev_warn(ctx->dev, "valid_bit_depth: src=%d dst=%d\n",
				src_fmt->valid_bit_depth, dst_fmt->valid_bit_depth);
		dev_warn(ctx->dev, "ch_cfg: src=%d dst=%d\n",
				src_fmt->ch_cfg, dst_fmt->ch_cfg);
		dev_warn(ctx->dev, "interleaving_style: src=%d dst=%d\n",
				src_fmt->interleaving_style, dst_fmt->interleaving_style);
		dev_warn(ctx->dev, "sample_type: src=%d dst=%d\n",
				src_fmt->sample_type, dst_fmt->sample_type);
		dev_warn(ctx->dev, "ch_map: src=%d dst=%d\n",
				src_fmt->ch_map, dst_fmt->ch_map);
	}
}

/*
 * Once a module is instantiated it need to be 'bind' with other modules in
 * the pipeline. For binding we need to find the module pins which are bind
 * together
 * This function finds the pins and then sends bund_unbind IPC message to
 * DSP using IPC helper
 */
int skl_bind_modules(struct skl_sst *ctx,
			struct skl_module_cfg *src_mcfg,
			struct skl_module_cfg *dst_mcfg)
{
	int ret = 0;
	struct skl_ipc_bind_unbind_msg msg;
	int in_max = dst_mcfg->module->max_input_pins;
	int out_max = src_mcfg->module->max_output_pins;
	int src_index, dst_index;
	struct skl_module_fmt *format;
	struct skl_cpr_pin_fmt pin_fmt;
	struct skl_module *module;
	struct skl_module_iface *fmt;

	skl_dump_bind_info(ctx, src_mcfg, dst_mcfg);

	if (src_mcfg->m_state < SKL_MODULE_INIT_DONE ||
		dst_mcfg->m_state < SKL_MODULE_INIT_DONE)
		return 0;

	src_index = skl_alloc_queue(src_mcfg->m_out_pin, dst_mcfg, out_max);
	if (src_index < 0)
		return -EINVAL;

	msg.src_queue = src_index;
	dst_index = skl_alloc_queue(dst_mcfg->m_in_pin, src_mcfg, in_max);
	if (dst_index < 0) {
		skl_free_queue(src_mcfg->m_out_pin, src_index);
		return -EINVAL;
	}

	/*
	 * Copier module requires the separate large_config_set_ipc to
	 * configure the pins other than 0
	 */
	if (src_mcfg->m_type == SKL_MODULE_TYPE_COPIER && src_index > 0) {
		pin_fmt.sink_id = src_index;
		module = src_mcfg->module;
		fmt = &module->formats[src_mcfg->fmt_idx];

		/* Input fmt is same as that of src module input cfg */
		format = &fmt->inputs[0].fmt;
		fill_pin_params(&(pin_fmt.src_fmt), format);

		format = &fmt->outputs[src_index].fmt;
		fill_pin_params(&(pin_fmt.dst_fmt), format);
		ret = skl_set_module_params(ctx, (void *)&pin_fmt,
					sizeof(struct skl_cpr_pin_fmt),
					CPR_SINK_FMT_PARAM_ID, src_mcfg);

		if (ret < 0)
			goto out;
	}

	msg.dst_queue = dst_index;

	dev_dbg(ctx->dev, "src queue = %d dst queue =%d\n",
			 msg.src_queue, msg.dst_queue);

	skl_module_format_mismatch_detection(ctx, src_mcfg, dst_mcfg,
						src_index, dst_index);

	msg.module_id = src_mcfg->id.module_id;
	msg.instance_id = src_mcfg->id.pvt_id;
	msg.dst_module_id = dst_mcfg->id.module_id;
	msg.dst_instance_id = dst_mcfg->id.pvt_id;
	msg.bind = true;

	ret = skl_ipc_bind_unbind(&ctx->ipc, &msg);

	if (!ret) {
		src_mcfg->m_state = SKL_MODULE_BIND_DONE;
		src_mcfg->m_out_pin[src_index].pin_state = SKL_PIN_BIND_DONE;
		dst_mcfg->m_in_pin[dst_index].pin_state = SKL_PIN_BIND_DONE;
		return ret;
	}
out:
	/* error case , if IPC fails, clear the queue index */
	skl_free_queue(src_mcfg->m_out_pin, src_index);
	skl_free_queue(dst_mcfg->m_in_pin, dst_index);

	return ret;
}

static int skl_set_pipe_state(struct skl_sst *ctx, struct skl_pipe *pipe,
	enum skl_ipc_pipeline_state state)
{
	dev_dbg(ctx->dev, "%s: pipe_satate = %d\n", __func__, state);

	return skl_ipc_set_pipeline_state(&ctx->ipc, pipe->ppl_id, state);
}

/*
 * A pipeline is a collection of modules. Before a module in instantiated a
 * pipeline needs to be created for it.
 * This function creates pipeline, by sending create pipeline IPC messages
 * to FW
 */
int skl_create_pipeline(struct skl_sst *ctx, struct skl_pipe *pipe)
{
	int ret;

	dev_dbg(ctx->dev, "%s: pipe_id = %d\n", __func__, pipe->ppl_id);

	ret = skl_ipc_create_pipeline(&ctx->ipc, pipe->memory_pages,
				pipe->pipe_priority, pipe->ppl_id,
				pipe->lp_mode);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to create pipeline\n");
		return ret;
	}

	pipe->state = SKL_PIPE_CREATED;
	skl_dbg_event(ctx, pipe->state);

	return 0;
}

/*
 * A pipeline needs to be deleted on cleanup. If a pipeline is running, then
 * pause the pipeline first and then delete it
 * The pipe delete is done by sending delete pipeline IPC. DSP will stop the
 * DMA engines and releases resources
 */
int skl_delete_pipe(struct skl_sst *ctx, struct skl_pipe *pipe)
{
	int ret;

	dev_dbg(ctx->dev, "%s: pipe = %d\n", __func__, pipe->ppl_id);

	/* If pipe is started, do stop the pipe in FW. */
	if (pipe->state >= SKL_PIPE_STARTED) {
		ret = skl_set_pipe_state(ctx, pipe, PPL_PAUSED);
		if (ret < 0) {
			dev_err(ctx->dev, "Failed to stop pipeline\n");
			return ret;
		}

		pipe->state = SKL_PIPE_PAUSED;
	}

	/* If pipe was not created in FW, do not try to delete it */
	if (pipe->state < SKL_PIPE_CREATED)
		return 0;

	ret = skl_ipc_delete_pipeline(&ctx->ipc, pipe->ppl_id);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to delete pipeline\n");
		return ret;
	}

	pipe->state = SKL_PIPE_INVALID;
	skl_dbg_event(ctx, pipe->state);
	ret = skl_notify_tplg_change(ctx, SKL_TPLG_CHG_NOTIFY_PIPELINE_DELETE);
	if (ret < 0)
		dev_warn(ctx->dev,
			"update of topology event delete pipe failed\n");

	return ret;
}

/*
 * A pipeline is also a scheduling entity in DSP which can be run, stopped
 * For processing data the pipe need to be run by sending IPC set pipe state
 * to DSP
 */
int skl_run_pipe(struct skl_sst *ctx, struct skl_pipe *pipe)
{
	int ret;

	dev_dbg(ctx->dev, "%s: pipe = %d\n", __func__, pipe->ppl_id);

	/* If pipe was not created in FW, do not try to pause or delete */
	if (pipe->state < SKL_PIPE_CREATED)
		return 0;

	/* Pipe has to be paused before it is started */
	ret = skl_set_pipe_state(ctx, pipe, PPL_PAUSED);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to pause pipe\n");
		return ret;
	}

	pipe->state = SKL_PIPE_PAUSED;

	ret = skl_set_pipe_state(ctx, pipe, PPL_RUNNING);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to start pipe\n");
		return ret;
	}

	pipe->state = SKL_PIPE_STARTED;
	ret = skl_notify_tplg_change(ctx, SKL_TPLG_CHG_NOTIFY_PIPELINE_START);
	if (ret < 0)
		dev_warn(ctx->dev,
			"update of topology event run pipe failed\n");

	return 0;
}

/*
 * Stop the pipeline by sending set pipe state IPC
 * DSP doesnt implement stop so we always send pause message
 */
int skl_stop_pipe(struct skl_sst *ctx, struct skl_pipe *pipe)
{
	int ret;

	dev_dbg(ctx->dev, "In %s pipe=%d\n", __func__, pipe->ppl_id);

	/* If pipe was not created in FW, do not try to pause or delete */
	if (pipe->state < SKL_PIPE_PAUSED)
		return 0;

	ret = skl_set_pipe_state(ctx, pipe, PPL_PAUSED);
	if (ret < 0) {
		dev_dbg(ctx->dev, "Failed to stop pipe\n");
		return ret;
	}

	pipe->state = SKL_PIPE_PAUSED;

	return 0;
}

/*
 * Reset the pipeline by sending set pipe state IPC this will reset the DMA
 * from the DSP side
 */
int skl_reset_pipe(struct skl_sst *ctx, struct skl_pipe *pipe)
{
	int ret;

	/* If pipe was not created in FW, do not try to pause or delete */
	if (pipe->state < SKL_PIPE_PAUSED)
		return 0;

	ret = skl_set_pipe_state(ctx, pipe, PPL_RESET);
	if (ret < 0) {
		dev_dbg(ctx->dev, "Failed to reset pipe ret=%d\n", ret);
		return ret;
	}

	pipe->state = SKL_PIPE_RESET;

	return 0;
}

/* Algo parameter set helper function */
int skl_set_module_params(struct skl_sst *ctx, u32 *params, int size,
				u32 param_id, struct skl_module_cfg *mcfg)
{
	struct skl_ipc_large_config_msg msg;

	msg.module_id = mcfg->id.module_id;
	msg.instance_id = mcfg->id.pvt_id;
	msg.param_data_size = size;
	msg.large_param_id = param_id;

	return skl_ipc_set_large_config(&ctx->ipc, &msg, params);
}

int skl_get_module_params(struct skl_sst *ctx, u32 *params, int size,
			  u32 param_id, struct skl_module_cfg *mcfg)
{
	struct skl_ipc_large_config_msg msg;

	msg.module_id = mcfg->id.module_id;
	msg.instance_id = mcfg->id.pvt_id;
	msg.param_data_size = size;
	msg.large_param_id = param_id;

	return skl_ipc_get_large_config(&ctx->ipc, &msg, params, NULL,
			0, NULL);
}
