	.set noreorder

#include "pspstub.s"

	STUB_START "I18N",0x00090000,0x00020005
	STUB_FUNC  0x4EC881B4,i18nGetText
	STUB_FUNC  0xAC13DCB0,i18nFreeMemory
	STUB_END
