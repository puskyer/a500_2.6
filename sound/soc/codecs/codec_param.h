/*
** ===============================
**	Audio codec WM8903 gain configuration
** ===============================
*/

enum project_id {
	A500 = 0,      //A500(PICASSO)
	SL101,         //SL101(EP102)
};

struct wm8903_parameters{
	u8 analog_speaker_volume;
	u8 analog_headset_volume;
	u8 analog_DMIC_ADC_volume;
	u8 analog_headset_mic_volume;
};


