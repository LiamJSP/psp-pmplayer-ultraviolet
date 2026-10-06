	.set noreorder

#include "pspimport.s"

	IMPORT_START	"sceAudio_driver",0x00010000
	IMPORT_FUNC	"sceAudio_driver",0xA2BEAA6C,sceAudioSetFrequency
	IMPORT_FUNC	"sceAudio_driver",0x442588EB,sceAudioSetFrequency371
	IMPORT_FUNC	"sceAudio_driver",0x9A34BC0D,sceAudioSetFrequency380
	IMPORT_FUNC	"sceAudio_driver",0xD1B9083A,sceAudioSetFrequency395
	IMPORT_FUNC	"sceAudio_driver",0x9DB844C6,sceAudioSetFrequency500
	IMPORT_FUNC	"sceAudio_driver",0x45543440,sceAudioSetFrequency620
	IMPORT_FUNC	"sceAudio_driver",0x0BF0FA03,sceAudioSetFrequency635
	IMPORT_FUNC	"sceAudio_driver",0x4A0FE97D,sceAudioSetFrequency660