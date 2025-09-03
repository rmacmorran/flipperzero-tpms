# Flipper Zero TPMS App - Firmware Compatibility Updates

This document outlines the changes made to update the TPMS application for compatibility with the latest Flipper Zero firmware, resolving the "Err_06 Outdated App" error.

## Changes Made

### 1. Application Manifest (application.fam)
- **Updated**: Added `"subghz"` to the requires list
- **Updated**: Added `fap_libs=["subghz"]` for proper library linking
- **Updated**: Bumped version from "0.1" to "1.0"

### 2. SubGHz API Migration
Replaced deprecated `furi_hal_subghz_*` functions with modern `subghz_devices_*` API:

**In `tpms_app_i.c`:**
- `furi_hal_subghz_reset()` → `subghz_devices_reset(device)`
- `furi_hal_subghz_idle()` → `subghz_devices_idle(device)`
- `furi_hal_subghz_load_custom_preset()` → `subghz_devices_load_preset(device, preset, data)`
- `furi_hal_subghz_is_frequency_valid()` → `subghz_devices_is_frequency_valid(device, freq)`
- `furi_hal_subghz_set_frequency_and_path()` → `subghz_devices_set_frequency(device, freq)`
- `furi_hal_subghz_flush_rx()` → `subghz_devices_flush_rx(device)`
- `furi_hal_subghz_start_async_rx()` → `subghz_devices_start_async_rx(device, callback, worker)`
- `furi_hal_subghz_stop_async_rx()` → `subghz_devices_stop_async_rx(device)`
- `furi_hal_subghz_get_rssi()` → `subghz_devices_get_rssi(device)`
- `furi_hal_subghz_sleep()` → `subghz_devices_sleep(device)`

### 3. Device API Updates
**In `tpms_app_i.h`:**
- Added `#include <lib/subghz/devices/devices.h>` for new device API

**In `tpms_app.c`:**
- Fixed radio device initialization bug where uninitialized pointer was passed
- Changed `radio_device_loader_set(app->txrx->radio_device, ...)` to `radio_device_loader_set(NULL, ...)`

### 4. Preset Handling Improvements
**In `tpms_app_i.c` (tpms_begin function):**
- Added null check for preset_data
- Added fallback to default OOK650 preset when custom preset data is not provided
- Removed unused UNUSED(preset_data) macro since we now actually use the parameter

## Key Compatibility Fixes

1. **Device-based API**: All SubGHz operations now go through the device abstraction layer
2. **Proper initialization**: Fixed uninitialized radio device pointer
3. **Library dependencies**: Added proper subghz library requirements
4. **Fallback presets**: Improved handling when custom preset data is unavailable

## Potential Remaining Issues

1. **RFID HAL API**: The relearn feature still uses `furi_hal_rfid_tim_read_*` functions which may need updating in future firmware versions
2. **Protocol Registry**: The custom protocol registry might need adjustments if the SubGhz protocol API changes further

## Testing Instructions

To test the updated application:

1. **Build the application** using the Flipper Zero build tools:
   ```bash
   cd flipperzero-tpms
   fbt fap_dist
   ```

2. **Install on Flipper Zero**:
   - Copy the generated `.fap` file from `dist/f7-C/` to your Flipper's `apps/Sub-GHz/` directory
   - Or use qFlipper to install directly

3. **Test basic functionality**:
   - Launch the TPMS app from Apps > Sub-GHz > TPMS
   - Verify it no longer shows "Err_06 Outdated App"
   - Test scanning for TPMS signals
   - Test the relearn functionality (Right button near a TPMS sensor)

4. **Verify features work**:
   - Check that RSSI display updates properly
   - Verify that detected sensors appear in the list
   - Test navigation and configuration screens
   - Confirm temperature and pressure readings are accurate

## Notes for Future Updates

- Keep monitoring Flipper Zero firmware releases for further API changes
- The SubGHz device API is more stable than the direct HAL API
- Consider migrating any remaining HAL calls to device-based APIs
- Monitor protocol decoder API for potential changes

## Build Dependencies

Ensure you have the latest Flipper Zero firmware source code and build environment set up correctly. The app now requires:
- Flipper Zero firmware 0.98.0 or later
- Proper SubGHz library support
- CC1101 device drivers (internal and external)

## Version History

- v1.0: Updated for modern Flipper Zero firmware compatibility
  - Migrated from furi_hal_subghz_* to subghz_devices_* API
  - Fixed device initialization issues
  - Updated build configuration
  - Improved preset handling
