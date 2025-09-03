#pragma once

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include "tpms_generic.h"
#include <lib/subghz/blocks/math.h>

#define TPMS_PROTOCOL_GM_NAME "GM TPMS"

typedef struct TPMSProtocolDecoderGM TPMSProtocolDecoderGM;
typedef struct TPMSProtocolEncoderGM TPMSProtocolEncoderGM;

extern const SubGhzProtocolDecoder tpms_protocol_gm_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_gm_encoder;
extern const SubGhzProtocol tpms_protocol_gm;

/**
 * Allocate TPMSProtocolDecoderGM.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return TPMSProtocolDecoderGM* pointer to a TPMSProtocolDecoderGM instance
 */
void* tpms_protocol_decoder_gm_alloc(SubGhzEnvironment* environment);

/**
 * Free TPMSProtocolDecoderGM.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 */
void tpms_protocol_decoder_gm_free(void* context);

/**
 * Reset decoder TPMSProtocolDecoderGM.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 */
void tpms_protocol_decoder_gm_reset(void* context);

/**
 * Parse a raw sequence of levels and durations received from the air.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 * @param level Signal level true-high false-low
 * @param duration Duration of this level in, us
 */
void tpms_protocol_decoder_gm_feed(void* context, bool level, uint32_t duration);

/**
 * Getting the hash sum of the last randomly received parcel.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 * @return hash Hash sum
 */
uint8_t tpms_protocol_decoder_gm_get_hash_data(void* context);

/**
 * Serialize data TPMSProtocolDecoderGM.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @param preset The modulation on which the signal was received, SubGhzRadioPreset
 * @return status
 */
SubGhzProtocolStatus tpms_protocol_decoder_gm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data TPMSProtocolDecoderGM.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    tpms_protocol_decoder_gm_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Getting a textual representation of the received data.
 * @param context Pointer to a TPMSProtocolDecoderGM instance
 * @param output Resulting text
 */
void tpms_protocol_decoder_gm_get_string(void* context, FuriString* output);
