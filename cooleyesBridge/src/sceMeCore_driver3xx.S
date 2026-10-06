	.set noreorder

#include "pspimport.s"

	IMPORT_START	"sceMeCore_driver",0x00010000
	IMPORT_FUNC	"sceMeCore_driver",0x47DB48C2,sceMeBootStart
	IMPORT_FUNC	"sceMeCore_driver",0xC287AD90,sceMeBootStart371
	IMPORT_FUNC	"sceMeCore_driver",0xD857CF93,sceMeBootStart380
	IMPORT_FUNC	"sceMeCore_driver",0x8988AD49,sceMeBootStart395
	IMPORT_FUNC	"sceMeCore_driver",0x051C1601,sceMeBootStart500
	IMPORT_FUNC	"sceMeCore_driver",0x3A2E60BB,sceMeBootStart620
	IMPORT_FUNC	"sceMeCore_driver",0x99E4DBFA,sceMeBootStart635
	IMPORT_FUNC	"sceMeCore_driver",0x5DFF5C50,sceMeBootStart660