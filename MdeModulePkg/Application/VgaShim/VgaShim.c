/** @file
 VgaShim Code

**/


#include "LegacyVgaBios.h"
#include "Int10hHandler.h"
#include "VgaShim.h"
#include "bootflag.h"


BOOLEAN							GraphicsInitialized = FALSE;
BOOLEAN							GraphicsFound = FALSE;
EFI_UGA_DRAW_PROTOCOL			*UgaDraw;
EFI_GRAPHICS_OUTPUT_PROTOCOL	*GraphicsOutput;
VIDEO_INFO						VideoInfo;


/**
  The entry point for the application.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       VGA ROM shim has been installed successfully
                            or it was found not to be required.
  @retval other             Some error occured during execution.

**/
EFI_STATUS
EFIAPI
UefiMain (
	IN EFI_HANDLE		ImageHandle,
	IN EFI_SYSTEM_TABLE	*SystemTable)
{
	EFI_PHYSICAL_ADDRESS	Int10hHandlerAddress;
	IVT_ENTRY				*Int10hHandlerEntry;
	EFI_PHYSICAL_ADDRESS	TempAddress;
	EFI_STATUS				Status;
	EFI_INPUT_KEY			Key;
	IMAGE					*WindowsFlag;
	UINTN					index;

	//Print(L"VGA Shim v0.7\n");
	//PrintVideoInfo();

	Status = gST->ConOut->Reset(gST->ConOut, FALSE);
	//Status = gST->ConIn->Reset(gST->ConIn, FALSE);
	//Print(L"Waiting for key stroke but you should not be seeing this message\n");
	
	ClearScreen();
	WindowsFlag = BmpFileToImage(bootflag, sizeof bootflag);
	DrawImage(WindowsFlag, 600, 600);

	do {
		gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
		gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

		if (Key.ScanCode != SCAN_NULL) {
			//ClearScreen();
			DrawImage(WindowsFlag, 600, 600);
		}

	} while (Key.ScanCode != SCAN_ESC);


	Print(L"End of waiting\n");

	//
	// If an Int10h handler exists there either is a real
	// VGA ROM in operation or we installed the shim before.
	//
	if (IsInt10HandlerDefined()) {
		Print(L"%a: Int10h already has a handler, you should be all set\n", __FUNCTION__);
		goto Exit;
	}

	//
	// Sanity checks; this should never creep into production.
	//
	ASSERT(sizeof INT10H_HANDLER <= VGA_ROM_SIZE);

	//
	// Unlock VGA ROM memory for writing first.
	//
	Status = EnsureMemoryLock(VGA_ROM_ADDRESS, VGA_ROM_SIZE, MEM_UNLOCK);
	if (EFI_ERROR(Status)) {
		Print(L"%a: Unable to unlock VGA ROM memory at %x for shim insertion\n", 
			__FUNCTION__, VGA_ROM_ADDRESS);
		goto Exit;
	}

	//
	// Claim real mode IVT memory area. This can be done as the IDT
	// has already been initialized so we can overwrite the IVT.
	//
	Int10hHandlerEntry = (IVT_ENTRY *)IVT_ADDRESS + 0x10;
	Print(L"%a: Claiming IVT area ... ", __FUNCTION__);
	TempAddress = IVT_ADDRESS;
	Status = gBS->AllocatePages(AllocateAddress, EfiBootServicesCode, 1, &TempAddress);
	if (EFI_ERROR(Status)) {
		Print(L"failure: %r\n", Status);
		return EFI_ABORTED;
	} else {
		Print(L"success\n");
	}

	//
	// Copy ROM stub in place and fill in the missing information.
	//
	SetMem((VOID *)VGA_ROM_ADDRESS, VGA_ROM_SIZE, 0);
	CopyMem((VOID *)VGA_ROM_ADDRESS, INT10H_HANDLER, sizeof INT10H_HANDLER);
	Status = FillVesaInformation(VGA_ROM_ADDRESS, &Int10hHandlerAddress);
	if (EFI_ERROR(Status)) {
		Print(L"%a: Cannot complete shim installation, aborting\n", __FUNCTION__);
		return EFI_ABORTED;
	} else {
		Print(L"%a: VESA information filled in, Int10h handler address = %x\n", 
			__FUNCTION__, Int10hHandlerAddress);
	}
	
	//
	// Lock the VGA ROM memory to prevent further writes.
	//
	Status = EnsureMemoryLock(VGA_ROM_ADDRESS, VGA_ROM_SIZE, MEM_LOCK);
	if (EFI_ERROR(Status)) {
		Print(L"%a: Unable to lock VGA ROM memory at %x but this is not essential\n", 
			__FUNCTION__, VGA_ROM_ADDRESS);
	}

	//
	// Point the Int10h vector at the entry point in shim.
	//
	// Convert from real 32bit physical address to real mode segment address
	Int10hHandlerEntry->Segment = (UINT16)((UINT32)VGA_ROM_ADDRESS >> 4);
	Int10hHandlerEntry->Offset = (UINT16)(Int10hHandlerAddress - VGA_ROM_ADDRESS);
	Print(L"%a: Int10h handler installed at %04x:%04x\n",
		__FUNCTION__, Int10hHandlerEntry->Segment, Int10hHandlerEntry->Offset);

Exit:
	Print(L"%a: Done!\n", __FUNCTION__);
	// clear screen
	// show image
	return EFI_SUCCESS;
}


/**
  Prints important information about the currently running video
  mode. Initializes adapters if they have not yet been detected.

**/
VOID
PrintVideoInfo()
{
	if (!GraphicsInitialized) {
		InitializeGraphics();
	}

	Print(L"%a: HorizontalResolution = %u\n", __FUNCTION__, VideoInfo.HorizontalResolution);
	Print(L"%a: VerticalResolution = %u\n", __FUNCTION__, VideoInfo.VerticalResolution);
	Print(L"%a: PixelFormat = %u\n", __FUNCTION__, VideoInfo.PixelFormat);
	Print(L"%a: PixelsPerScanLine = %u\n", __FUNCTION__, VideoInfo.PixelsPerScanLine);
	Print(L"%a: FrameBufferBase = %x\n", __FUNCTION__, VideoInfo.FrameBufferBase);
	Print(L"%a: FrameBufferSize = %u\n", __FUNCTION__, VideoInfo.FrameBufferSize);
}


/**
  Scans the system for Graphics Output Protocol (GOP) and
  Universal Graphic Adapter (UGA) compatible adapters/GPUs.
  If one is found, vital information about its video mode is
  retrieved and stored for later use.

  @retval EFI_SUCCESS     An adapter was found and its current
                          mode parameters stored in VideoInfo
						  global variable.
  @retval other           No compatible adapters were found or
                          their mode parameters could not be
						  retrieved.
  
**/
EFI_STATUS
InitializeGraphics()
{
	EFI_STATUS	Status;
	UINT32		Temp1;
	UINT32		Temp2;

	if (GraphicsInitialized) {
		return EFI_SUCCESS;
	}

	UgaDraw			= NULL;
	GraphicsOutput	= NULL;

	SetMem(&VideoInfo, sizeof(VIDEO_INFO), 0);

	//
	// Try a GOP adapter first.
	//
	Status = gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&GraphicsOutput);
	if (!EFI_ERROR(Status)) {
		Print(L"%a: Found a GOP protocol provider\n", __FUNCTION__);
		VideoInfo.HorizontalResolution = GraphicsOutput->Mode->Info->HorizontalResolution;
		VideoInfo.VerticalResolution = GraphicsOutput->Mode->Info->VerticalResolution;
		VideoInfo.PixelFormat = GraphicsOutput->Mode->Info->PixelFormat;
		VideoInfo.PixelsPerScanLine = GraphicsOutput->Mode->Info->PixelsPerScanLine;
		VideoInfo.FrameBufferBase = GraphicsOutput->Mode->FrameBufferBase;
		// usually = PixelsPerScanLine * VerticalResolution * BytesPerPixel
		// for MacBookAir7,2: 1536 * 900 * 4 = 5,529,600 bytes
		VideoInfo.FrameBufferSize = GraphicsOutput->Mode->FrameBufferSize;
		
		goto Exit;
	}

	//
	// Try a UGA adapter.
	//
	GraphicsOutput = NULL;
	Status = gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiUgaDrawProtocolGuid, (VOID **)&UgaDraw);
	if (!EFI_ERROR(Status)) {
		Print(L"%a: Found a UGA protocol provider\n", __FUNCTION__);
		Status = UgaDraw->GetMode(UgaDraw, &VideoInfo.HorizontalResolution, &VideoInfo.VerticalResolution, &Temp1, &Temp2);
		if (EFI_ERROR(Status)) {
			Print(L"%a: Unable to get current UGA mode\n", __FUNCTION__);
			goto Exit;
		}

		VideoInfo.PixelFormat = PixelBlueGreenRedReserved8BitPerColor; // default for UGA
		// TODO: find framebuffer base
		// TODO: find scanline length
		// https://github.com/coreos/grub/blob/master/grub-core%2Fvideo%2Fefi_uga.c
	}

Exit:
	GraphicsFound = !EFI_ERROR(Status);
	GraphicsInitialized = TRUE;
	return Status;
}


/**
  Fills in VESA-compatible information about supported video modes
  in the space left for this purpose at the beginning of the 
  generated VGA ROM assembly code.

  @param[in] StartAddress Where to begin writing VESA information.
  @param[in] EndAddress   Pointer to the next byte after the end
                          of all video mode information data.

  @retval EFI_SUCCESS     The operation was successful
  @return other           The operation failed.

**/
EFI_STATUS
FillVesaInformation(
	IN	EFI_PHYSICAL_ADDRESS	StartAddress,
	OUT	EFI_PHYSICAL_ADDRESS	*EndAddress)
{
	// (Page 26 in VESA BIOS EXTENSION Core Functions Standard v3.0.)
	// (Page 30 in VESA BIOS EXTENSION Core Functions Standard v3.0.)

	VBE_INFO				*VbeInfoFull;
	VBE_INFO_BASE			*VbeInfo;
	VBE_MODE_INFO			*VbeModeInfo;
	UINT8					*BufferPtr;
	UINT32					HorizontalOffsetPx;
	UINT32					VerticalOffsetPx;
	EFI_PHYSICAL_ADDRESS	FrameBufferBaseWithOffset;

	//
	// Get basic video hardware information first.
	//
	if (!GraphicsInitialized) {
		InitializeGraphics();
	}
	if (!GraphicsFound) {
		Print(L"%a: No adapters were found, unable to fill in VESA information\n", __FUNCTION__);
		return EFI_NOT_FOUND;
	}
	
	//
	// VESA general information.
	//
	VbeInfoFull = (VBE_INFO *)(UINTN)StartAddress;
	VbeInfo = &VbeInfoFull->Base;
	BufferPtr = VbeInfoFull->Buffer;
	CopyMem(VbeInfo->Signature, "VESA", 4);
	VbeInfo->VesaVersion = 0x0300;
	VbeInfo->OemNameAddress = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
	CopyMem(BufferPtr, VENDOR_NAME, sizeof VENDOR_NAME);
	BufferPtr += sizeof VENDOR_NAME;
	VbeInfo->Capabilities = BIT0;			// DAC width supports 8-bit color mode
	VbeInfo->ModeListAddress = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
	*(UINT16*)BufferPtr = 0x00f1;			// mode number
	BufferPtr += 2;
	*(UINT16*)BufferPtr = 0xFFFF;			// mode list terminator
	BufferPtr += 2;
	VbeInfo->VideoMem64K = (UINT16)((VideoInfo.FrameBufferSize + 65535) / 65536);
	VbeInfo->OemSoftwareVersion = 0x0000;
	VbeInfo->VendorNameAddress = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
	CopyMem(BufferPtr, VENDOR_NAME, sizeof VENDOR_NAME);
	BufferPtr += sizeof VENDOR_NAME;
	VbeInfo->ProductNameAddress = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
	CopyMem(BufferPtr, PRODUCT_NAME, sizeof PRODUCT_NAME);
	BufferPtr += sizeof PRODUCT_NAME;
	VbeInfo->ProductRevAddress = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
	CopyMem(BufferPtr, PRODUCT_REVISION, sizeof PRODUCT_REVISION);
	BufferPtr += sizeof PRODUCT_REVISION;
	
	// make sure we did not use more buffer than we had space for
	ASSERT(sizeof VbeInfoFull->Buffer >= BufferPtr - VbeInfoFull->Buffer);
	
	//
	// Basic VESA mode information.
	//
	VbeModeInfo = (VBE_MODE_INFO *)(VbeInfoFull + 1); // jump ahead by sizeof(VBE_INFO) ie. 256
	// bit0: mode supported by present hardware configuration
	// bit1: must be set for VBE v1.2+
	// bit3: color mode
	// bit4: graphics mode
	// bit5: mode not VGA-compatible (do not access VGA I/O ports and registers)
	// bit6: disable windowed memory mode = linear framebuffer only
	// bit7: linear framebuffer supported
	VbeModeInfo->ModeAttr = BIT7 | BIT6 | BIT5 | BIT4 | BIT3 | BIT1 | BIT0;
	
	//
	// Resolution.
	//
	VbeModeInfo->Width = 1024;						// as expected by Windows installer
	VbeModeInfo->Height = 768;						// as expected by Windows installer
	VbeModeInfo->CharCellWidth = 8;					// used to calculate resolution in text modes
	VbeModeInfo->CharCellHeight = 16;				// used to calculate resolution in text modes
	
	// Calculate offsets so that the smaller image appears centered on the screen
	HorizontalOffsetPx = (VideoInfo.HorizontalResolution - 1024) / 2;
	VerticalOffsetPx = (VideoInfo.VerticalResolution - 768) / 2 * VideoInfo.PixelsPerScanLine;
	FrameBufferBaseWithOffset = VideoInfo.FrameBufferBase 
		+ VerticalOffsetPx * 4
		+ HorizontalOffsetPx * 4;

	//
	// Memory access (banking, windowing, paging).
	//
	VbeModeInfo->NumBanks = 1;						// disable memory banking
	VbeModeInfo->BankSizeKB = 0;					// disable memory banking
	VbeModeInfo->LfbAddress = 
		(UINT32)FrameBufferBaseWithOffset;			// 32-bit physical address
	VbeModeInfo->BytesPerScanLineLinear = 
		VideoInfo.PixelsPerScanLine * 4;			// logical bytes in linear modes
	VbeModeInfo->NumImagePagesLessOne = 0;			// disable image paging
	VbeModeInfo->NumImagesLessOneLinear = 0;		// disable image paging
	VbeModeInfo->WindowPositioningAddress = 0x0;	// force windowing to Function 5h
	VbeModeInfo->WindowAAttr = 0x0;					// window disabled
	VbeModeInfo->WindowBAttr = 0x0;					// window disabled
	VbeModeInfo->WindowGranularityKB = 0x0;			// window disabled and not relocatable
	VbeModeInfo->WindowSizeKB = 0x0;				// set to 64KB even thou window is disabled
	VbeModeInfo->WindowAStartSegment = 0x0;			// linear framebuffer only
	VbeModeInfo->WindowBStartSegment = 0x0;			// linear framebuffer only

	//
	// Color mode
	// 
	VbeModeInfo->NumPlanes = 1;						// packed pixel mode
	VbeModeInfo->MemoryModel = 6;					// Direct Color
	VbeModeInfo->DirectColorModeInfo = BIT1;		// alpha bytes may be used by application
	VbeModeInfo->BitsPerPixel = 32;					// 8+8+8+8 bits per channel
	VbeModeInfo->BlueMaskSizeLinear = 8;
	VbeModeInfo->GreenMaskSizeLinear = 8;
	VbeModeInfo->RedMaskSizeLinear = 8;
	VbeModeInfo->ReservedMaskSizeLinear = 8;

	if (VideoInfo.PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
		VbeModeInfo->BlueMaskPosLinear = 0;			// blue offset
		VbeModeInfo->GreenMaskPosLinear = 8;		// green offset
		VbeModeInfo->RedMaskPosLinear = 16;			// green offset
		VbeModeInfo->ReservedMaskPosLinear = 24;	// reserved offset
	} else if (VideoInfo.PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
		VbeModeInfo->RedMaskPosLinear = 0;			// red offset
		VbeModeInfo->GreenMaskPosLinear = 8;		// green offset
		VbeModeInfo->BlueMaskPosLinear = 16;		// blue offset
		VbeModeInfo->ReservedMaskPosLinear = 24;	// alpha offset
	} else {
		Print(L"%a: Unsupported value of PixelFormat (%d), aborting\n", 
			__FUNCTION__, VideoInfo.PixelFormat);
		return EFI_UNSUPPORTED;
	}
	
	//
	// Other
	//
	VbeModeInfo->OffScreenAddress = 0;				// reserved, always set to 0
	VbeModeInfo->OffScreenSizeKB = 0;				// reserved, always set to 0
	VbeModeInfo->MaxPixelClockHz = 0;				// maximum available refresh rate
	VbeModeInfo->Vbe3 = 0x01;						// reserved, always set to 1

	*EndAddress = (UINTN)(VbeModeInfo + 1);			// jump ahead by sizeof(VBE_MODE_INFO) ie. 256
	return EFI_SUCCESS;
}


/**
  Checkes if an Int10h handler is already defined in the
  Interrupt Vector Table (IVT).

  @retval TRUE            An Int10h handler was found in IVT.
  @retval FALSE           An Int10h handler was not found in IVT.

**/
BOOLEAN
IsInt10HandlerDefined()
{
	IN	IVT_ENTRY				*Int10Entry;
	IN	EFI_PHYSICAL_ADDRESS	Int10Handler;
	
	// (convert from real mode segment address to 32bit physical address)
	Int10Entry = (IVT_ENTRY *)(UINTN)IVT_ADDRESS + 0x10;
	Int10Handler = (Int10Entry->Segment << 4) + Int10Entry->Offset;

	Print(L"%a: Checking for an existing Int10h handler ... ", __FUNCTION__);

	if (Int10Handler >= VGA_ROM_ADDRESS && Int10Handler < (VGA_ROM_ADDRESS+VGA_ROM_SIZE)) {
		Print(L"found at %04x:%04x\n", Int10Entry->Segment, Int10Entry->Offset);
		return TRUE;
	} else {
		Print(L"not found\n");
		return FALSE;
	}
}


/**
  Attempts to either unlock a memory area for writing or
  lock it to prevent writes. Makes use of a number of approaches
  to achieve the desired result.

  @param[in] StartAddress Where the desired memory area begins.
  @param[in] Length       Number of bytes from StartAddress that
                          need to be locked or unlocked.
  @param[in] Operation    Whether the area is to be locked or unlocked. 

  @retval TRUE            An Int10h handler was found in IVT.
  @retval FALSE           An Int10h handler was not found in IVT.

**/
EFI_STATUS
EnsureMemoryLock(
	IN	EFI_PHYSICAL_ADDRESS	StartAddress,
	IN	UINT32					Length,
	IN	MEMORY_LOCK_OPERATION	Operation)
{
	EFI_STATUS					Status = EFI_NOT_READY;
	UINT32						Granularity;
	EFI_LEGACY_REGION_PROTOCOL	*mLegacyRegion = NULL;
	EFI_LEGACY_REGION2_PROTOCOL	*mLegacyRegion2 = NULL;

	//
	// Check if we need to perform any operation.
	// 
	if (Operation == MEM_UNLOCK && CanWriteAtAddress(StartAddress)) {
		Print(L"%a: Memory at %x already unlocked\n", __FUNCTION__, StartAddress);
		Status = EFI_SUCCESS;
	} else if (Operation == MEM_LOCK && !CanWriteAtAddress(StartAddress)) {
		Print(L"%a: Memory at %x already locked\n", __FUNCTION__, StartAddress);
		Status = EFI_SUCCESS;
	}

	//
	// Try to lock/unlock with EfiLegacyRegionProtocol.
	// 
	if (EFI_ERROR(Status)) {
		Status = gBS->LocateProtocol(&gEfiLegacyRegionProtocolGuid, NULL, (VOID **)&mLegacyRegion);
		if (!EFI_ERROR(Status)) {
			if (Operation == MEM_UNLOCK) {
				Status = mLegacyRegion->UnLock(mLegacyRegion, (UINT32)StartAddress, Length, &Granularity);
				Status = CanWriteAtAddress(StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
			} else {
				Status = mLegacyRegion->Lock(mLegacyRegion, (UINT32)StartAddress, Length, &Granularity);
				Status = CanWriteAtAddress(StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
			}

			Print(L"%a: %s %s memory at %x using EfiLegacyRegionProtocol\n", 
				__FUNCTION__, 
				EFI_ERROR(Status) ? L"Failure" : L"Success",
				Operation == MEM_UNLOCK ? L"unlocking" : L"locking", 
				StartAddress);
		}
	}
	
	//
	// Try to lock/unlock with EfiLegacyRegion2Protocol.
	//
	if (EFI_ERROR(Status)) {
		Status = gBS->LocateProtocol(&gEfiLegacyRegion2ProtocolGuid, NULL, (VOID **)&mLegacyRegion2);
		if (!EFI_ERROR(Status)) {
			if (Operation == MEM_UNLOCK) {
				Status = mLegacyRegion2->UnLock(mLegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
				Status = CanWriteAtAddress(StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;;
			} else {
				Status = mLegacyRegion2->Lock(mLegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
				Status = CanWriteAtAddress(StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
			}

			Print(L"%a: %s %s memory at %x using EfiLegacyRegion2Protocol\n", 
				__FUNCTION__, 
				EFI_ERROR(Status) ? L"Failure" : L"Success",
				Operation == MEM_UNLOCK ? L"unlocking" : L"locking", 
				StartAddress);
		}
	}
	
	//
	// Try to lock/unlock via an MTRR.
	//
	if (EFI_ERROR(Status) && IsMtrrSupported()) {
		ASSERT(FIXED_MTRR_SIZE >= Length);
		if (Operation == MEM_UNLOCK) {
			MtrrSetMemoryAttribute(StartAddress, FIXED_MTRR_SIZE, CacheUncacheable);
			Status = CanWriteAtAddress(StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
		} else {
			MtrrSetMemoryAttribute(StartAddress, FIXED_MTRR_SIZE, CacheWriteProtected);
			Status = CanWriteAtAddress(StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
		}

		Print(L"%a: %s %s memory at %x using MTRR\n", 
			__FUNCTION__, 
			EFI_ERROR(Status) ? "Failure" : "Success",
			Operation == MEM_UNLOCK ? "unlocking" : "locking", 
			StartAddress);
	}
	
	//
	// None of the methods worked?
	// 
	if (EFI_ERROR(Status)) {
		Print(L"%a: Unable to find a way to %s memory at %x\n", 
			__FUNCTION__, Operation == MEM_UNLOCK ? "unlock" : "lock", StartAddress);
	}
	
	return Status;
}


/**
  Checks if writes are possible in a particular memory area.

  @param[in] Address      The memory location to be checked.

  @retval TRUE            Writes to the specified location are
                          allowed changes are persisted.
  @retval FALSE           Writes to the specified location are
                          not allowed or have no effect.
  
**/
BOOLEAN
CanWriteAtAddress(
	IN	EFI_PHYSICAL_ADDRESS	Address)
{
	BOOLEAN	CanWrite;
	UINT8	*TestPtr;
	UINT8	OldValue;

	TestPtr = (UINT8*)(Address);
	OldValue = *TestPtr;
	
	*TestPtr = *TestPtr + 1;
	CanWrite = OldValue != *TestPtr;
	
	*TestPtr = OldValue;
	return CanWrite;
}


VOID
ClearScreen()
{
    EFI_UGA_PIXEL	FillColor;
	//EFI_CONSOLE_CONTROL_SCREEN_MODE Mode;
	FillColor.Red		= 0xff;
	FillColor.Green		= 0xf;
	FillColor.Blue		= 0xf;
	FillColor.Reserved	= 0xf;

	if (!GraphicsInitialized) {
		InitializeGraphics();
	}

	//ConsoleControl->SetMode(ConsoleControl, EfiConsoleControlScreenGraphics);

    if (GraphicsOutput != NULL) {
		GraphicsOutput->Blt(
			GraphicsOutput, 
			(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&FillColor,
			EfiBltVideoFill,
			0, 0, 0, 0,
			VideoInfo.HorizontalResolution, VideoInfo.VerticalResolution, 0);
    } else if (UgaDraw != NULL) {
        UgaDraw->Blt(
			UgaDraw, 
			&FillColor, 
			EfiUgaVideoFill, 
			0, 0, 0, 0, 
			VideoInfo.HorizontalResolution, VideoInfo.VerticalResolution, 0);
    } else {
	    Print(L"%a: No graphics device found, unable to clear screen\n", __FUNCTION__);
    }
}


VOID
DestroyImage(
	IN	IMAGE	*Image)
{
	if (Image != NULL) {
        if (Image->PixelData != NULL)
            FreePool(Image->PixelData);
        FreePool(Image);
    }
}


IMAGE*
CreateImage(
	IN	UINTN	Width,
	IN	UINTN	Height)
{
	IMAGE	*Image;

	Image = (IMAGE *)AllocatePool(sizeof(IMAGE));
	if (Image == NULL) {
		return NULL;
	}
		
	Image->Width = Width;
	Image->Height = Height;
	Image->PixelData = (EFI_UGA_PIXEL *)AllocatePool(Width * Height * sizeof(EFI_UGA_PIXEL));
	if (Image->PixelData == NULL) {
		DestroyImage(Image);
		return NULL;
	}
	
	SetMem(Image->PixelData, sizeof Image->PixelData, 0);
	return Image;
}


VOID 
DrawImage(
	IN	IMAGE	*Image,
	IN	UINTN	PosX,
	IN	UINTN	PosY)
{
	EFI_STATUS Status;
	EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Random;

	if (!GraphicsInitialized) {
		InitializeGraphics();
	}
	if (!GraphicsFound) {
		Print(L"%a: No graphics device found, unable to draw image\n", __FUNCTION__);
		return;
	}
	if (Image == NULL || Image->Width == 0 || Image->Height == 0) {
		Print(L"%a: No image specified\n", __FUNCTION__);
		return;
	}

	if ((PosX + Image->Width) > VideoInfo.HorizontalResolution 
		|| (PosY + Image->Height) > VideoInfo.VerticalResolution) {
		Print(L"%a: Image too big to draw on screen\n", __FUNCTION__);
		return;
	}

	Random = AllocatePool(600 * 600 * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
	SetMem(Random, 600 * 600 * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL), 6);

	if (GraphicsOutput != NULL) {
		Print(L"%a: Outputting image to a GOP device\n", __FUNCTION__);
		Status = GraphicsOutput->Blt(
			GraphicsOutput, Random, EfiBltBufferToVideo, 
			0, 0, 300, 300, 600, 600, 600 * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
		Print(L"%a: Status = %r\n", __FUNCTION__, Status);
	} else if (UgaDraw != NULL) {
		Print(L"%a: Outputting image to a UGA device\n", __FUNCTION__);
		UgaDraw->Blt(UgaDraw, 
			(EFI_UGA_PIXEL *)Image->PixelData, 
			EfiUgaBltBufferToVideo,
			0, 0, PosX, PosY, 
			Image->Width, Image->Height, 0);
	} else {
		Print(L"%a: No graphics device found, unable to draw image\n", __FUNCTION__);
	}
	
	//DestroyImage(Image);
}


VOID
LoadFile(
	IN	EFI_HANDLE		ImageHandle)
{
	EFI_STATUS					Status;
	EFI_LOADED_IMAGE_PROTOCOL	*ImageInfo;
	Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&ImageInfo);
	if (!EFI_ERROR(Status)) {
		Print(L"FilePath=%x\n", ImageInfo->FilePath);
		Print(L"ImageSize=%u\n", ImageInfo->ImageSize);
		Print(L"LoadOptionsSize=%u\n", ImageInfo->LoadOptionsSize);
		Print(L"Revision=%u\n", ImageInfo->Revision);
		Print(L"Image base: %lx\n", ImageInfo->ImageBase);
		Print(L"Image file: %s\n", ConvertDevicePathToText(ImageInfo->FilePath, TRUE, FALSE));
		Print(L"DevicePathType: %u\n", DevicePathType(ImageInfo->FilePath));
		Print(L"DevicePathSubType: %u\n", DevicePathSubType(ImageInfo->FilePath));
		Print(L"NextDevicePathNode: %s\n", ConvertDevicePathToText(NextDevicePathNode(ImageInfo->FilePath), TRUE, FALSE));
		//ImageInfo->DeviceHandle ROOT of device
	}
	return;
}









IMAGE*
BmpFileToImage(
	IN	UINT8	*FileData,
	IN	UINTN	FileSizeBytes)
{
	IMAGE			*Image;
	BMP_HEADER		*BmpHeader;
	UINT8			*BmpCurrentPixel;
	UINT8			*BmpCurrentLine;
	UINTN			LineSizeBytes;
	EFI_UGA_PIXEL	*TargetPixel;
	UINTN			x, y;

	// Sanity checks.
	if (FileData == NULL || FileSizeBytes < sizeof(BMP_HEADER)) {
		Print(L"%a: File too small or does not exist, aborting\n", __FUNCTION__);
		return NULL;
	}

	BmpHeader = (BMP_HEADER *)FileData;
	if (BmpHeader->Signature[0] != 'B' 
		|| BmpHeader->Signature[1] != 'M'
		|| BmpHeader->CompressionType != 0	// only support uncompressed...
		|| BmpHeader->BitPerPixel != 24		// ...24 bits per pixel images
		|| BmpHeader->Width < 1
		|| BmpHeader->Height < 1) {
		return NULL;
	}
		
	Image = CreateImage(BmpHeader->Width, BmpHeader->Height);
	if (Image == NULL) {
		Print(L"%a: Unable to create image, aborting\n", __FUNCTION__);
		return NULL;
	}
	
	// Calculate line size and adjust with padding to multiple of 4 bytes.
	LineSizeBytes = BmpHeader->Width * 3; // 24 bits = 3 bytes
	LineSizeBytes += (LineSizeBytes % 4) != 0
		? (4 - (LineSizeBytes % 4))
		: 0;
	
	// Check if we have enough pixel data.
	if (BmpHeader->PixelDataOffset + BmpHeader->Height * LineSizeBytes > FileSizeBytes) {
		Print(L"%a: Not enough pixel data, aborting\n", __FUNCTION__);
		DestroyImage(Image);
		return NULL;
	}
		

	// Fill in pixel values.
	BmpCurrentLine = FileData + BmpHeader->PixelDataOffset;
	for (y = 0; y < BmpHeader->Height; y++) {
		BmpCurrentPixel = BmpCurrentLine;
		BmpCurrentLine += LineSizeBytes;
		// jump to the right pixel line; BMP PixelArray is bottom-to-top...
		TargetPixel = Image->PixelData + BmpHeader->Width * (BmpHeader->Height - y - 1);
		// ...but thankfully left-to-right
		for (x = 0; x < BmpHeader->Width; x++) {
			TargetPixel->Blue		= *BmpCurrentPixel++;
			TargetPixel->Blue		= 128;
			TargetPixel->Green		= *BmpCurrentPixel++;
			TargetPixel->Red		= *BmpCurrentPixel++;
			TargetPixel->Reserved	= 128;
			TargetPixel++;
		}
	}

	Print(L"%a: Done creating image size %u x %u from bmp\n", __FUNCTION__, Image->Width, Image->Height);
	return Image;
}