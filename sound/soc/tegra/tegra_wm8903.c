/*
 * tegra_wm8903.c - Tegra machine ASoC driver for boards using WM8903 codec.
 *
 * Author: Stephen Warren <swarren@nvidia.com>
 * Copyright (C) 2010-2011 - NVIDIA, Inc.
 *
 * Based on code copyright/by:
 *
 * (c) 2009, 2010 Nvidia Graphics Pvt. Ltd.
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif

#if !defined(CONFIG_ARCH_ACER_T20) && !defined(CONFIG_ARCH_ACER_T30)
#include <mach/tegra_wm8903_pdata.h>
#endif

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/wm8903.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
#include "tegra20_das.h"
#endif

#if defined(CONFIG_ARCH_ACER_T20)
#include "acer_audio_control_t20.h"
#elif defined(CONFIG_ARCH_ACER_T30)
#include "acer_audio_control_t30.h"
#endif

#define DRV_NAME "tegra-snd-wm8903"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)
#define GPIO_INT_MIC_EN BIT(2)
#define GPIO_EXT_MIC_EN BIT(3)
#define GPIO_HP_DET     BIT(4)
#ifdef CONFIG_ARCH_ACER_T30
#define GPIO_BYBASS_EN  BIT(5)
#endif
#ifdef CONFIG_MACH_VANGOGH
#define GPIO_SPKR_MUTE  BIT(6)
#endif

struct tegra_wm8903 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_wm8903_platform_data *pdata;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
	int gpio_requested;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
};

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
struct acer_audio_data audio_data;

#if defined(CONFIG_ARCH_ACER_T30)
void acer_set_bypass_switch(int state)
{
	gpio_set_value(audio_data.gpio.bypass_en, state);
	pr_info("audio: acer audio bypass switch state = %d \n",
			gpio_get_value(audio_data.gpio.bypass_en));
}
#endif
#endif

static int tegra_wm8903_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, i2s_daifmt;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	i2s_daifmt = SND_SOC_DAIFMT_NB_NF |
		     SND_SOC_DAIFMT_CBS_CFS;

	/* Use DSP mode for mono on Tegra20 */
	if ((params_channels(params) != 2) &&
	    (machine_is_ventana() || machine_is_harmony() ||
	    machine_is_kaen() || machine_is_aebl()))
		i2s_daifmt |= SND_SOC_DAIFMT_DSP_A;
	else
		i2s_daifmt |= SND_SOC_DAIFMT_I2S;

	err = snd_soc_dai_set_fmt(codec_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai, i2s_daifmt);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
					SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	err = tegra20_das_connect_dac_to_dap(TEGRA20_DAS_DAP_SEL_DAC1,
					TEGRA20_DAS_DAP_ID_1);
	if (err < 0) {
		dev_err(card->dev, "failed to set dap-dac path\n");
		return err;
	}

	err = tegra20_das_connect_dap_to_dac(TEGRA20_DAS_DAP_ID_1,
					TEGRA20_DAS_DAP_SEL_DAC1);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}
#endif

#if defined(CONFIG_ARCH_ACER_T20)
	acer_volume_setting(codec, substream);
#endif
	return 0;
}

static int tegra_bt_sco_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 64 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	err = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_DSP_A |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	err = tegra20_das_connect_dac_to_dap(TEGRA20_DAS_DAP_SEL_DAC2,
					TEGRA20_DAS_DAP_ID_4);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}

	err = tegra20_das_connect_dap_to_dac(TEGRA20_DAS_DAP_ID_4,
					TEGRA20_DAS_DAP_SEL_DAC2);
	if (err < 0) {
		dev_err(card->dev, "failed to set dac-dap path\n");
		return err;
	}
#endif
	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	int srate, mclk, min_mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 64000:
	case 96000:
		mclk = 12288000;
		break;
	default:
		return -EINVAL;
	}
	min_mclk = 128 * srate;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		if (!(machine->util_data.set_mclk % min_mclk))
			mclk = machine->util_data.set_mclk;
		else {
			dev_err(card->dev, "Can't configure clocks\n");
			return err;
		}
	}

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 1);

	return 0;
}

static int tegra_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(rtd->card);

	tegra_asoc_utils_lock_clk_rate(&machine->util_data, 0);

	return 0;
}

static struct snd_soc_ops tegra_wm8903_ops = {
	.hw_params = tegra_wm8903_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_wm8903_bt_sco_ops = {
	.hw_params = tegra_bt_sco_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
	.hw_free = tegra_hw_free,
};

static struct snd_soc_jack tegra_wm8903_hp_jack;
static struct snd_soc_jack tegra_wm8903_mic_jack;

static struct snd_soc_jack_gpio tegra_wm8903_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
#if defined(CONFIG_ARCH_ACER_T30)
	.invert = 0,
#else
	.invert = 1,
#endif
};

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_wm8903_headset_switch = {
	.name = "h2w",
};

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
int get_headset_state(void)
{
	return switch_get_state(&tegra_wm8903_headset_switch);
}

int acer_soc_suspend_pre(struct snd_soc_card *card)
{
#if defined(CONFIG_MACH_VANGOGH)
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;
	gpio_set_value_cansleep(pdata->gpio_spkr_en, 0);
#endif
	return 0;
}

int acer_soc_resume_post(struct snd_soc_card *card)
{
#if defined(CONFIG_MACH_VANGOGH)
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;
#endif
	int state = (int) is_hp_plugged();

	if (!is_debug_on())
		snd_soc_jack_report(&tegra_wm8903_hp_jack, state, 1);

#if defined(CONFIG_MACH_VANGOGH)
	gpio_set_value_cansleep(pdata->gpio_spkr_en, 1);
#endif
	return 0;
}
#endif

static int tegra_wm8903_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	enum headset_state state = BIT_NO_HEADSET;

	if (jack == &tegra_wm8903_hp_jack) {
		machine->jack_status &= ~SND_JACK_HEADPHONE;
		machine->jack_status |= (action & SND_JACK_HEADPHONE);
	} else {
		machine->jack_status &= ~SND_JACK_MICROPHONE;
		machine->jack_status |= (action & SND_JACK_MICROPHONE);
	}

	switch (machine->jack_status) {
#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	case SND_JACK_HEADPHONE:
	case SND_JACK_HEADSET:
		if (handset_mic_detect(codec)) {
			state = BIT_HEADSET;
		} else {
			state = BIT_HEADSET_NO_MIC;
		}
		break;
#else
	case SND_JACK_HEADPHONE:
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_HEADSET:
		state = BIT_HEADSET;
		break;
#endif
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_wm8903_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_wm8903_jack_detect_nb = {
	.notifier_call = tegra_wm8903_jack_notifier,
};
#else
static struct snd_soc_jack_pin tegra_wm8903_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static struct snd_soc_jack_pin tegra_wm8903_mic_jack_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
};
#endif

static int tegra_wm8903_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	wm8903_event_printf(__func__, event);
#endif

	if (machine->spk_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			regulator_enable(machine->spk_reg);
		else
			regulator_disable(machine->spk_reg);
	}

	if (!(machine->gpio_requested & GPIO_SPKR_EN))
		return 0;

#if defined(CONFIG_MACH_VANGOGH)
	gpio_set_value_cansleep(pdata->gpio_spkr_mute,
				!SND_SOC_DAPM_EVENT_ON(event));
#else
	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				SND_SOC_DAPM_EVENT_ON(event));
#endif

	return 0;
}

static int tegra_wm8903_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	wm8903_event_printf(__func__, event);
#endif

	if (!(machine->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

#ifdef CONFIG_ARCH_ACER_T30
static int tegra_wm8903_event_dock_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

	wm8903_event_printf(__func__, event);

	return 0;
}
#endif

static int tegra_wm8903_event_int_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	wm8903_event_printf(__func__, event);
#endif


	if (machine->dmic_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			regulator_enable(machine->dmic_reg);
		else
			regulator_disable(machine->dmic_reg);
	}

	if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
		return 0;

#if defined(CONFIG_ARCH_ACER_T20)
	set_int_mic_state(SND_SOC_DAPM_EVENT_ON(event) ? true : false);
	tune_codec_setting(audio_data.mode.input_source);
	fm2018_switch(pdata);
#else
	gpio_set_value_cansleep(pdata->gpio_int_mic_en,
				SND_SOC_DAPM_EVENT_ON(event));
#endif

	return 0;
}

static int tegra_wm8903_event_ext_mic(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	wm8903_event_printf(__func__, event);
#endif

#if defined(CONFIG_ARCH_ACER_T20)
	if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
		return 0;

	set_ext_mic_state(SND_SOC_DAPM_EVENT_ON(event) ? true : false);
	tune_codec_setting(audio_data.mode.input_source);
	fm2018_switch(pdata);
#else
	if (!(machine->gpio_requested & GPIO_EXT_MIC_EN))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_ext_mic_en,
				SND_SOC_DAPM_EVENT_ON(event));
#endif

	return 0;
}

static const struct snd_soc_dapm_widget cardhu_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_wm8903_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_wm8903_event_hp),
#ifdef CONFIG_ARCH_ACER_T30
	SND_SOC_DAPM_LINE("Line Out", tegra_wm8903_event_dock_hp),
#else
	SND_SOC_DAPM_LINE("Line Out", NULL),
#endif
	SND_SOC_DAPM_MIC("Mic Jack", tegra_wm8903_event_ext_mic),
	SND_SOC_DAPM_MIC("Int Mic", tegra_wm8903_event_int_mic),
	SND_SOC_DAPM_LINE("Line In", NULL),
};

static const struct snd_soc_dapm_widget tegra_wm8903_default_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", tegra_wm8903_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", tegra_wm8903_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
};

static const struct snd_soc_dapm_route harmony_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1L", NULL, "Mic Bias"},
};

#if defined(CONFIG_ARCH_ACER_T20)
static const struct snd_soc_dapm_route cardhu_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Line Out", NULL, "ROP"},
	{"Line Out", NULL, "RON"},
	{"Line Out", NULL, "LOP"},
	{"Line Out", NULL, "LON"},
	{"Int Spk", NULL, "LINEOUTL"},
	{"Int Spk", NULL, "LINEOUTR"},
	{"Mic Bias", NULL, "Mic Jack"},
#ifdef CONFIG_ACER_FM_SINGLE_MIC
	{"IN1L", NULL, "Mic Jack"},
	{"IN1R", NULL, "Mic Jack"},
#else
	{"IN2L", NULL, "Mic Jack"},
	{"IN2R", NULL, "Mic Jack"},
#endif
	{"Mic Bias", NULL, "Int Mic"},
	{"IN1L", NULL, "Int Mic"},
	{"IN1R", NULL, "Int Mic"},
	{"IN3L", NULL, "Line In"},
	{"IN3R", NULL, "Line In"},
};
#elif defined(CONFIG_ARCH_ACER_T30)
static const struct snd_soc_dapm_route cardhu_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"LineOut", NULL, "Line Out"},
	{"Line Out", NULL, "LINEOUTL"},
	{"Line Out", NULL, "LINEOUTR"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1L", NULL, "Mic Jack"},
	{"IN1L", NULL, "Mic Jack"},
	{"Mic Bias", NULL, "Int Mic"},
	{"IN2L", NULL, "Int Mic"},
	{"IN2R", NULL, "Int Mic"},
	{"IN3L", NULL, "Int Mic"},
	{"IN3R", NULL, "Int Mic"},
};
#else
static const struct snd_soc_dapm_route cardhu_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Line Out", NULL, "LINEOUTL"},
	{"Line Out", NULL, "LINEOUTR"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1L", NULL, "Mic Bias"},
	{"Mic Bias", NULL, "Int Mic"},
	{"IN1L", NULL, "Mic Bias"},
	{"IN1R", NULL, "Mic Bias"},
	{"IN3L", NULL, "Line In"},
	{"IN3R", NULL, "Line In"},
};
#endif

static const struct snd_soc_dapm_route seaboard_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1R", NULL, "Mic Bias"},
};

static const struct snd_soc_dapm_route kaen_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN2R", NULL, "Mic Bias"},
};

static const struct snd_soc_dapm_route aebl_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "LINEOUTR"},
	{"Int Spk", NULL, "LINEOUTL"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1R", NULL, "Mic Bias"},
};

static const struct snd_kcontrol_new cardhu_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("LineOut"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("LineIn"),
#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	SOC_DAPM_SET_PARAM("audio_source"),
	SOC_DAPM_SET_PARAM("ap_id"),
#endif
};

static const struct snd_kcontrol_new tegra_wm8903_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
        SOC_DAPM_PIN_SWITCH("Mic Jack"),
};

static int tegra_wm8903_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;
	int ret;

	if (gpio_is_valid(pdata->gpio_spkr_en)) {
		ret = gpio_request(pdata->gpio_spkr_en, "spkr_en");
		if (ret) {
			dev_err(card->dev, "cannot get spkr_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_SPKR_EN;
#if defined(CONFIG_MACH_VANGOGH)
		gpio_direction_output(pdata->gpio_spkr_en, 1);
#else
		gpio_direction_output(pdata->gpio_spkr_en, 0);
#endif
	}

	if (gpio_is_valid(pdata->gpio_hp_mute)) {
		ret = gpio_request(pdata->gpio_hp_mute, "hp_mute");
		if (ret) {
			dev_err(card->dev, "cannot get hp_mute gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_HP_MUTE;

		gpio_direction_output(pdata->gpio_hp_mute, 0);
	}

	if (gpio_is_valid(pdata->gpio_int_mic_en)) {
		ret = gpio_request(pdata->gpio_int_mic_en, "int_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get int_mic_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_INT_MIC_EN;

		/* Disable int mic; enable signal is active-high */
		gpio_direction_output(pdata->gpio_int_mic_en, 0);
	}

	if (gpio_is_valid(pdata->gpio_ext_mic_en)) {
		ret = gpio_request(pdata->gpio_ext_mic_en, "ext_mic_en");
		if (ret) {
			dev_err(card->dev, "cannot get ext_mic_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_EXT_MIC_EN;

		/* Enable ext mic; enable signal is active-low */
		gpio_direction_output(pdata->gpio_ext_mic_en, 0);
	}

#if defined(CONFIG_ARCH_ACER_T30)
	/* bypass switch enable gpio */
	if (gpio_is_valid(pdata->gpio_bypass_switch_en)) {
		tegra_gpio_enable(pdata->gpio_bypass_switch_en);
		ret = gpio_request(pdata->gpio_bypass_switch_en, "bypass_switch_en");
		if (ret) {
			dev_err(card->dev, "cannot get bypass_switch_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_BYBASS_EN;

		/* Enable bypass switch; enable signal is active-high */
		gpio_direction_output(pdata->gpio_bypass_switch_en, 1);
	}
#endif

#if defined(CONFIG_MACH_VANGOGH)
	if (gpio_is_valid(pdata->gpio_spkr_mute)) {
		ret = gpio_request(pdata->gpio_spkr_mute, "spkr_mute");
		if (ret) {
			dev_err(card->dev, "cannot get spkr_mute gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_SPKR_MUTE;

		gpio_direction_output(pdata->gpio_spkr_mute, 1);
	}
#endif

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	ret = snd_soc_add_controls(codec, cardhu_controls,
			ARRAY_SIZE(cardhu_controls));
	if (ret < 0)
		return ret;
	snd_soc_dapm_new_controls(dapm, cardhu_dapm_widgets,
			ARRAY_SIZE(cardhu_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, cardhu_audio_map,
			ARRAY_SIZE(cardhu_audio_map));
#else
	if (machine_is_cardhu() || machine_is_ventana()) {
		ret = snd_soc_add_controls(codec, cardhu_controls,
				ARRAY_SIZE(cardhu_controls));
		if (ret < 0)
			return ret;

		snd_soc_dapm_new_controls(dapm, cardhu_dapm_widgets,
				ARRAY_SIZE(cardhu_dapm_widgets));
	}
	else {
		ret = snd_soc_add_controls(codec,
				tegra_wm8903_default_controls,
				ARRAY_SIZE(tegra_wm8903_default_controls));
		if (ret < 0)
			return ret;

		snd_soc_dapm_new_controls(dapm,
				tegra_wm8903_default_dapm_widgets,
				ARRAY_SIZE(tegra_wm8903_default_dapm_widgets));
	}

	if (machine_is_harmony()) {
		snd_soc_dapm_add_routes(dapm, harmony_audio_map,
					ARRAY_SIZE(harmony_audio_map));
	} else if (machine_is_cardhu() || machine_is_ventana()) {
		snd_soc_dapm_add_routes(dapm, cardhu_audio_map,
					ARRAY_SIZE(cardhu_audio_map));
	} else if (machine_is_seaboard()) {
		snd_soc_dapm_add_routes(dapm, seaboard_audio_map,
					ARRAY_SIZE(seaboard_audio_map));
	} else if (machine_is_kaen()) {
		snd_soc_dapm_add_routes(dapm, kaen_audio_map,
					ARRAY_SIZE(kaen_audio_map));
	} else {
		snd_soc_dapm_add_routes(dapm, aebl_audio_map,
					ARRAY_SIZE(aebl_audio_map));
	}
#endif

#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	audio_data.gpio.debug_en = pdata->gpio_debug_switch_en;
	/* if debug on, will not enable headset */
	if (gpio_is_valid(pdata->gpio_hp_det) && !is_debug_on()) {
		pr_info("[Audio]register headphone jack\n");
		audio_data.gpio.hp_det = pdata->gpio_hp_det;
#else
	if (gpio_is_valid(pdata->gpio_hp_det)) {
#endif
		tegra_wm8903_hp_jack_gpio.gpio = pdata->gpio_hp_det;
		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
				&tegra_wm8903_hp_jack);
#ifndef CONFIG_SWITCH
		snd_soc_jack_add_pins(&tegra_wm8903_hp_jack,
					ARRAY_SIZE(tegra_wm8903_hp_jack_pins),
					tegra_wm8903_hp_jack_pins);
#else
		snd_soc_jack_notifier_register(&tegra_wm8903_hp_jack,
					&tegra_wm8903_jack_detect_nb);
#endif
		snd_soc_jack_add_gpios(&tegra_wm8903_hp_jack,
					1,
					&tegra_wm8903_hp_jack_gpio);
		machine->gpio_requested |= GPIO_HP_DET;
	}

	snd_soc_jack_new(codec, "Mic Jack", SND_JACK_MICROPHONE,
			 &tegra_wm8903_mic_jack);

#if !defined(CONFIG_ARCH_ACER_T20) && !defined(CONFIG_ARCH_ACER_T30)
#ifndef CONFIG_SWITCH
	snd_soc_jack_add_pins(&tegra_wm8903_mic_jack,
			      ARRAY_SIZE(tegra_wm8903_mic_jack_pins),
			      tegra_wm8903_mic_jack_pins);
#else
	snd_soc_jack_notifier_register(&tegra_wm8903_mic_jack,
				&tegra_wm8903_jack_detect_nb);
#endif
#endif
	wm8903_mic_detect(codec, &tegra_wm8903_mic_jack, SND_JACK_MICROPHONE,
			  machine_is_cardhu() ? SND_JACK_MICROPHONE : 0);

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias");

	/* Disable Spk,Headphone,Line Out when wm8903 init */
#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	snd_soc_dapm_disable_pin(dapm, "Int Spk");
	snd_soc_dapm_disable_pin(dapm, "Headphone Jack");
	snd_soc_dapm_disable_pin(dapm, "Line Out");

	snd_soc_dapm_disable_pin(dapm, "Mic Jack");
	snd_soc_dapm_disable_pin(dapm, "Int Mic");
	snd_soc_dapm_disable_pin(dapm, "Line In");

	audio_data.codec = codec;
	audio_data.gpio.spkr_en = pdata->gpio_spkr_en;
#if defined(CONFIG_ARCH_ACER_T20)
	audio_data.gpio.int_mic_en = pdata->gpio_int_mic_en;
#elif defined(CONFIG_ARCH_ACER_T30)
	audio_data.gpio.bypass_en = pdata->gpio_bypass_switch_en;
#endif
#if defined(CONFIG_MACH_VANGOGH)
	audio_data.gpio.spkr_mute = pdata->gpio_spkr_mute;
#endif
#else
	/* FIXME: Calculate automatically based on DAPM routes? */
	if (!machine_is_harmony() && !machine_is_ventana() &&
	    !machine_is_cardhu())
		snd_soc_dapm_nc_pin(dapm, "IN1L");
	if (!machine_is_seaboard() && !machine_is_aebl() &&
	    !machine_is_cardhu())
		snd_soc_dapm_nc_pin(dapm, "IN1R");
	snd_soc_dapm_nc_pin(dapm, "IN2L");
	if (!machine_is_kaen())
		snd_soc_dapm_nc_pin(dapm, "IN2R");
	snd_soc_dapm_nc_pin(dapm, "IN3L");
	snd_soc_dapm_nc_pin(dapm, "IN3R");

	if (machine_is_aebl()) {
		snd_soc_dapm_nc_pin(dapm, "LON");
		snd_soc_dapm_nc_pin(dapm, "RON");
		snd_soc_dapm_nc_pin(dapm, "ROP");
		snd_soc_dapm_nc_pin(dapm, "LOP");
	} else {
		snd_soc_dapm_nc_pin(dapm, "LINEOUTR");
		snd_soc_dapm_nc_pin(dapm, "LINEOUTL");
	}
#endif

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link tegra_wm8903_dai[] = {
	{
		.name = "WM8903",
		.stream_name = "WM8903 PCM",
		.codec_name = "wm8903.0-001a",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.0",
		.codec_dai_name = "wm8903-hifi",
		.init = tegra_wm8903_init,
		.ops = &tegra_wm8903_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	},
	{
		.name = "BT-SCO",
		.stream_name = "BT SCO PCM",
		.codec_name = "spdif-dit.1",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.1",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_wm8903_bt_sco_ops,
	},
};

static struct snd_soc_card snd_soc_tegra_wm8903 = {
	.name = "tegra-wm8903",
	.dai_link = tegra_wm8903_dai,
	.num_links = ARRAY_SIZE(tegra_wm8903_dai),
#if defined(CONFIG_ARCH_ACER_T20) || defined(CONFIG_ARCH_ACER_T30)
	.suspend_pre = acer_soc_suspend_pre,
	.resume_post = acer_soc_resume_post,
#endif
};

static __devinit int tegra_wm8903_driver_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_tegra_wm8903;
	struct tegra_wm8903 *machine;
	struct tegra_wm8903_platform_data *pdata;
	int ret;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	machine = kzalloc(sizeof(struct tegra_wm8903), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_wm8903 struct\n");
		return -ENOMEM;
	}

	machine->pdata = pdata;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev);
	if (ret)
		goto err_free_machine;

	machine->spk_reg = regulator_get(&pdev->dev, "vdd_spk_amp");
	if (IS_ERR(machine->spk_reg)) {
		dev_info(&pdev->dev, "No speaker regulator found\n");
		machine->spk_reg = 0;
	}

	machine->dmic_reg = regulator_get(&pdev->dev, "vdd_dmic");
	if (IS_ERR(machine->dmic_reg)) {
		dev_info(&pdev->dev, "No digital mic regulator found\n");
		machine->dmic_reg = 0;
	}

#if defined(CONFIG_ARCH_ACER_T30)
	tegra_wm8903_dai[0].codec_name = "wm8903.4-001a",
	tegra_wm8903_dai[0].cpu_dai_name = "tegra30-i2s.1";
	tegra_wm8903_dai[1].cpu_dai_name = "tegra30-spdif";
	tegra_wm8903_dai[2].cpu_dai_name = "tegra30-i2s.3";
#else
	if (machine_is_cardhu()) {
		tegra_wm8903_dai[0].codec_name = "wm8903.4-001a",
		tegra_wm8903_dai[0].cpu_dai_name = "tegra30-i2s.1";

		tegra_wm8903_dai[1].cpu_dai_name = "tegra30-spdif";

		tegra_wm8903_dai[2].cpu_dai_name = "tegra30-i2s.3";
	}
#endif

#ifdef CONFIG_SWITCH
	/* Addd h2w swith class support */
	ret = switch_dev_register(&tegra_wm8903_headset_switch);
	if (ret < 0)
		goto err_fini_utils;
#endif

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_switch;
	}

	return 0;

err_unregister_switch:
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_wm8903_headset_switch);
err_fini_utils:
#endif
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
}

static int __devexit tegra_wm8903_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_wm8903 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_wm8903_platform_data *pdata = machine->pdata;

	if (machine->gpio_requested & GPIO_HP_DET)
		snd_soc_jack_free_gpios(&tegra_wm8903_hp_jack,
					1,
					&tegra_wm8903_hp_jack_gpio);
#if defined(CONFIG_MACH_VANGOGH)
	if (machine->gpio_requested & GPIO_SPKR_MUTE)
		gpio_free(pdata->gpio_spkr_mute);
#endif
#if defined(CONFIG_ARCH_ACER_T30)
	if (machine->gpio_requested & GPIO_BYBASS_EN)
		gpio_free(pdata->gpio_bypass_switch_en);
#endif
	if (machine->gpio_requested & GPIO_EXT_MIC_EN)
		gpio_free(pdata->gpio_ext_mic_en);
	if (machine->gpio_requested & GPIO_INT_MIC_EN)
		gpio_free(pdata->gpio_int_mic_en);
	if (machine->gpio_requested & GPIO_HP_MUTE)
		gpio_free(pdata->gpio_hp_mute);
	if (machine->gpio_requested & GPIO_SPKR_EN)
		gpio_free(pdata->gpio_spkr_en);
	machine->gpio_requested = 0;

	if (machine->spk_reg)
		regulator_put(machine->spk_reg);
	if (machine->dmic_reg)
		regulator_put(machine->dmic_reg);

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_wm8903_headset_switch);
#endif
	kfree(machine);

	return 0;
}

static struct platform_driver tegra_wm8903_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_wm8903_driver_probe,
	.remove = __devexit_p(tegra_wm8903_driver_remove),
};

static int __init tegra_wm8903_modinit(void)
{
	return platform_driver_register(&tegra_wm8903_driver);
}
module_init(tegra_wm8903_modinit);

static void __exit tegra_wm8903_modexit(void)
{
	platform_driver_unregister(&tegra_wm8903_driver);
}
module_exit(tegra_wm8903_modexit);

MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_DESCRIPTION("Tegra+WM8903 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
