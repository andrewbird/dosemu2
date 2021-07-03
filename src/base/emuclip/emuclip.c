#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "emu.h"
#include "dos2linux.h"
#include "doshelpers.h"

#include "clipboard.h"


#define UNKNOWN_COMMAND 3
#define ERROR       0x8000
#define DONE        0x0100
#define BUSY        0x0200
#define BLK_NOT_FOUND 8


static uint16_t emuclip_interrupt(uint16_t seg, uint16_t off)
{
  struct DDRH *rh = MK_FP32(seg, off);

  v_printf("EMUCLIP: Interrupt (%02d) called\n", rh->command);

  rh->status = 0;

  switch (rh->command) {
    case 0: /* driver initialization (Never called, implemented in asm) */
      assert(0);
      break;

    case 1: /* media check (Never called, block only) */
      assert(0);
      break;

    case 2: /* build parameter block (Never called, block only) */
      assert(0);
      break;

    case 4: /*  read */
    case 8: /*  write */
    case 9: /*  write with verify */
/*
      if (rh->io.start > MAX_BLK ||
          rh->io.count > MAX_BLK ||
          rh->io.start + rh->io.count > MAX_BLK) {
        rh->status = BLK_NOT_FOUND | ERROR;
        break;
      }
	*/

      v_printf("EMUCLIP: IO start(0x%04x), count(0x%04x)\n", rh->io.start, rh->io.count);

      /*
  char *p;
      if (rh->command == 4)	// read
        memcpy(FAR2PTR(rh->io.buf), p, BLK_SIZE * rh->io.count);
      else			// write
        memcpy(p, FAR2PTR(rh->io.buf), BLK_SIZE * rh->io.count);
	*/
      break;

    case 15: /*  removable media check */
      rh->status = BUSY;
      break;

    case 5:  /*  non-destructive read */
      rh->status = BUSY;
      break;

    case 6:  /*  input status */
    case 7:  /*  flush input buffers */
    case 10: /*  output status */
    case 11: /*  flush output buffers */
    case 13: /*  device open */
    case 14: /*  device done */
      break;

    case 3:  /*  ioctl read */
    case 12: /*  ioctl write */
    default:
      rh->status = UNKNOWN_COMMAND | ERROR;
      break;
  }

  return rh->status; // assembler caller merges DONE bit.
}

static int emuclip_drv_installed = 0;


void emuclip_helper(void)
{
  switch (HI(ax)) {
    /* Driver installation check. */
    case DOS_SUBHELPER_EMUCLIP_DRV_CHECK:
      LWORD(eax) = emuclip_drv_installed;
      v_printf("EMUCLIP: Driver installation check, AX=%d\n", LWORD(eax));
      break;

    /* Driver install. */
    case DOS_SUBHELPER_EMUCLIP_DRV_INSTALL:
      if (emuclip_drv_installed) {
        LWORD(ebx) = DOS_ERROR_EMUCLIP_ALREADY_INSTALLED;
        v_printf("EMUCLIP: Error driver already installed\n");
        CARRY;
        break;
      }

      if (config.clipboard == 0) {
        v_printf("EMUCLIP: Disabled in config\n");
        LWORD(ebx) = DOS_ERROR_EMUCLIP_CONFIG_DISABLED;
        CARRY;
        break;
      }

      emuclip_drv_installed = 1;
      NOCARRY;
      break;

    case DOS_SUBHELPER_EMUCLIP_DRV_INTERRUPT:
      LWORD(eax) = emuclip_interrupt(SREG(es), LWORD(edi));
      if (LWORD(eax))
        NOCARRY;
      else
        CARRY;
      break;

    default:
      v_printf("EMUCLIP: Error unknown subfunction 0x%02x!\n", HI(ax));
      break;
  }
}
