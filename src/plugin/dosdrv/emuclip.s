#
# emuclip.s : Clipboard driver for dosemu.
#
# Copyright (C) 2021 by Andrew Bird.
#
# The code in this module is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2 of
# the License, or (at your option) any later version.
#

#include "doshelpers.h"

# Common request header offsets
LEN	=	0
UNIT	=	1
CMD	=	2
STATUS	=	3

# Init only request header offsets
NUNITS  =	13
ENDOFS	=	14
ENDSEG	=	16
BPBPTROFF =	18
BPBPTRSEG =	20
FIRSTDRV =	22

# Media Check
MEDIAID =	13
CHANGED =	14


.text
.code16
	.globl	_start16
_start16:

# -------------------------------------------------------------------------
# Code/Data below is resident after install
# -------------------------------------------------------------------------
Header:
	.long	-1			# These 4 bytes are overwritten with
					# ptr to next driver at installation time
HdrAttr:
	.word   0b1110000000000000	# attribute word (char, ioctl, output til busy)
	.word	Strat			# ptr to strategy routine
	.word	Intr			# ptr to interrupt service routine
	.ascii  "CLIP$   "		# 8 bytes non significant padding
# Fields above are size and position sensitive

RhPtr:	.long	0			# ptr to request header

Dispatch:
	.word	Init		# initialize driver
	.word	Dosemu		# Media Check, block only
	.word	Dosemu		# Get BPB, block only
	.word	Dosemu		# Ioctl
	.word	Dosemu		# read
	.word	Dosemu 		# non-destructive read
	.word	Dosemu		# input status
	.word	Dosemu		# flush input
	.word	Dosemu		# write
	.word	Dosemu		# write with verify
	.word	Dosemu		# output status
	.word	Dosemu		# flush output
	.word	Dosemu		# IOCTL output (?)
/* if DOS 3.0 or newer... */
	.word	Dosemu		# open device
	.word	Dosemu		# close device
	.word	Dosemu		# removeable media check
Dispatch_End:

AmountCmd = (Dispatch_End - Dispatch) / 2

Strat:
	# Save address of request header
	movw	%bx, %cs:RhPtr
	movw	%es, %cs:RhPtr+2
	lret

Intr:
	pushf
	pusha
	pushw	%ds
	pushw	%es

	pushw	%cs
	popw	%ds

	les	RhPtr,%di		# let es:di = request header

	movb	%es:CMD(%di),%bl
	xorb	%bh,%bh
	cmpw	$AmountCmd,%bx
	jb	1f
	movw	$0x8003,%ax		# Set error
	jmp	2f

1:	shlw	%bx

	call	*Dispatch(%bx)

	les	RhPtr,%di

2:	or	$0x100,%ax		# Merge done bit with status
	movw	%ax,%es:STATUS(%di)

	popw	%es
	popw	%ds
	popa
	popf
	lret

Init:
	jmp	InitCode

Dosemu:
	# Notify dosemu module (ES:DI contains RHPtr)
	movb    $DOS_HELPER_EMUCLIP_HELPER,%al
	movb    $DOS_SUBHELPER_EMUCLIP_DRV_INTERRUPT,%ah
	int     $DOS_HELPER_INT
	# AX contains status without DONE bit
	ret

# -------------------------------------------------------------------------
# Code below is thrown away after Install
# -------------------------------------------------------------------------
InitCode:
#include "detect.h"

	pushw   %cs
	popw    %ds

	# notify dosemu module
	movw    $Header,%di
	pushw   %cs
	popw    %es
	movb    $DOS_HELPER_EMUCLIP_HELPER,%al
	movb    $DOS_SUBHELPER_EMUCLIP_DRV_INSTALL,%ah
	int     $DOS_HELPER_INT
	jnc	.Lgoodtoload

	cmpw	$DOS_ERROR_EMUCLIP_ALREADY_INSTALLED, %bx
	je	.Lalreadyloaded

	cmpw	$DOS_ERROR_EMUCLIP_CONFIG_DISABLED, %bx
	je	.Ldisabledconfig

	jmp	.Lunknownerror

.Lgoodtoload:
	movw    $installed_txt,%dx		# Show the message
	movb    $0x09,%ah
	int     $0x21

	les	RhPtr,%di
	movw	$InitCode, %es:ENDOFS(%di)	# End of resident driver
	movw	%cs, %es:ENDSEG(%di)
	xorw	%ax,%ax
	ret

.Lalreadyloaded:
	movw    $already_txt, %dx
	jmp	Error

.Ldisabledconfig:
	movw    $disabled_txt, %dx
	jmp	Error

.Lunknownerror:
	movw    $unknown_txt, %dx
#	jmp	Error

Error:
	movb    $0x09,%ah			# display error message (at ds:dx)
	int     $0x21

	movw	$0, %cs:HdrAttr			# Set to block type
	movb	$0, %es:NUNITS(%di)		# Units = 0
	movw	$0, %es:ENDOFS(%di)		# Break addr = cs:0000
	movw	%cs, %es:ENDSEG(%di)

	movw	$0x8003,%ax			# Set error
	ret

# -------------------------------------------------------------------------

installed_txt:
        .ascii  "Dosemu CLIP driver installed.\r\n$"

already_txt:
        .ascii  "Dosemu CLIP driver already installed.\r\n$"

disabled_txt:
        .ascii  "Dosemu CLIP driver disabled in config.\r\n$"

unknown_txt:
        .ascii  "Dosemu CLIP driver unknown error.\r\n$"
