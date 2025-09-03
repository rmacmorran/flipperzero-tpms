#pragma once

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include "tpms_generic.h"
#include <lib/subghz/blocks/math.h>

#define TPMS_PROTOCOL_TOYOTA_NAME "Toyota TPMS"

typedef struct TPMSProtocolDecoderToyota TPMSProtocolDecoderToyota;
typedef struct TPMSProtocolEncoderToyota TPMSProtocolEncoderToyota;

extern const SubGhzProtocolDecoder tpms_protocol_toyota_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_toyota_encoder;
extern const SubGhzProtocol tpms_protocol_toyota;

/**
 * Allocate TPMSProtocolDecoderToyota.
 * @param environment Pointer to a SubGhzEnvironment instance
 * @return TPMSProtocolDecoderToyota* pointer to a TPMSProtocolDecoderToyota instance
 */
void* tpms_protocol_decoder_toyota_alloc(SubGhzEnvironment* environment);

/**
 * Free TPMSProtocolDecoderToyota.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 */
void tpms_protocol_decoder_toyota_free(void* context);

/**
 * Reset decoder TPMSProtocolDecoderToyota.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 */
void tpms_protocol_decoder_toyota_reset(void* context);

/**
 * Parse a raw sequence of levels and durations received from the air.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 * @param level Signal level true-high false-low
 * @param duration Duration of this level in, us
 */
void tpms_protocol_decoder_toyota_feed(void* context, bool level, uint32_t duration);

/**
 * Getting the hash sum of the last randomly received parcel.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 * @return hash Hash sum
 */
uint8_t tpms_protocol_decoder_toyota_get_hash_data(void* context);

/**
 * Serialize data TPMSProtocolDecoderToyota.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @param preset The modulation on which the signal was received, SubGhzRadioPreset
 * @return status
 */
SubGhzProtocolStatus tpms_protocol_decoder_toyota_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

/**
 * Deserialize data TPMSProtocolDecoderToyota.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 * @param flipper_format Pointer to a FlipperFormat instance
 * @return status
 */
SubGhzProtocolStatus
    tpms_protocol_decoder_toyota_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Getting a textual representation of the received data.
 * @param context Pointer to a TPMSProtocolDecoderToyota instance
 * @param output Resulting text
 */
void tpms_protocol_decoder_toyota_get_string(void* context, FuriString* output);
