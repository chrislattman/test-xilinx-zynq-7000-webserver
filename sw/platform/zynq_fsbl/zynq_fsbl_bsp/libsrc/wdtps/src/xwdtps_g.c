#include "xwdtps.h"

XWdtPs_Config XWdtPs_ConfigTable[] __attribute__ ((section (".drvcfg_sec"))) = {

	{
		"cdns,wdt-r1p2", /* compatible */
		0xf8005000, /* reg */
		0x6750918, /* xlnx,wdt-clk-freq-hz */
		0x1009, /* interrupts */
		0xf8f01000 /* interrupt-parent */
	},
	 {
		 NULL
	}
};