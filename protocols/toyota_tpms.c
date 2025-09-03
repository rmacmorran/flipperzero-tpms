#include "toyota_tpms.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "Toyota TPMS"

/**
 * Toyota TPMS Protocol
 * 
 * Based on rtl_433 implementation:
 * - FSK modulation with differential Manchester encoding
 * - 315MHz frequency
 * - 9-byte packet structure
 * - CRC-8 with poly 0x07, init 0x80
 * 
 * Packet format (after differential Manchester decoding):
 * - Bytes 0-3: 32-bit sensor ID
 * - Byte 4: Status bit + pressure high bits
 * - Byte 5: Temperature + pressure bit  
 * - Byte 6: Status bits
 * - Byte 7: Inverted pressure (for validation)
 * - Byte 8: CRC-8
 * 
 * Temperature: offset by 40°C
 * Pressure: 1/4 PSI offset by -7 PSI (28 raw = 0 PSI)
 */

// Sync pattern: 0xa9, 0xe0 (12 bits)
static const uint8_t toyota_sync_pattern[] = {0xa9, 0xe0};

static const SubGhzBlockConst tpms_protocol_toyota_const = {
    .te_short = 52,     // FSK bit period ~52us for ~19.2kbps
    .te_long = 52,      // Same for FSK
    .te_delta = 15,     // Tolerance
    .min_count_bit_for_found = 72,  // 9 bytes * 8 bits
};

struct TPMSProtocolDecoderToyota {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    
    uint8_t manchester_data[10];  // Buffer for differential Manchester decoding
    uint8_t manchester_bit_count;
    bool looking_for_sync;
    uint16_t sync_found_bits;
};

struct TPMSProtocolEncoderToyota {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    ToyotaDecoderStepReset = 0,
    ToyotaDecoderStepSync,
    ToyotaDecoderStepData,
} ToyotaDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_toyota_decoder = {
    .alloc = tpms_protocol_decoder_toyota_alloc,
    .free = tpms_protocol_decoder_toyota_free,
    .feed = tpms_protocol_decoder_toyota_feed,
    .reset = tpms_protocol_decoder_toyota_reset,
    .get_hash_data = tpms_protocol_decoder_toyota_get_hash_data,
    .serialize = tpms_protocol_decoder_toyota_serialize,
    .deserialize = tpms_protocol_decoder_toyota_deserialize,
    .get_string = tpms_protocol_decoder_toyota_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_toyota_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_toyota = {
    .name = TPMS_PROTOCOL_TOYOTA_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &tpms_protocol_toyota_decoder,
    .encoder = &tpms_protocol_toyota_encoder,
};

void* tpms_protocol_decoder_toyota_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderToyota* instance = malloc(sizeof(TPMSProtocolDecoderToyota));
    instance->base.protocol = &tpms_protocol_toyota;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_toyota_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    free(instance);
}

void tpms_protocol_decoder_toyota_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    instance->decoder.parser_step = ToyotaDecoderStepReset;
    instance->manchester_bit_count = 0;
    instance->looking_for_sync = true;
    instance->sync_found_bits = 0;
}

static bool tpms_protocol_toyota_check_crc(uint8_t* data) {
    // CRC-8 with poly 0x07, init 0x80
    uint8_t crc = subghz_protocol_blocks_crc8(data, 8, 0x07, 0x80);
    return (crc == data[8]);
}

static void tpms_protocol_toyota_analyze(TPMSBlockGeneric* instance, uint8_t* data) {
    // Extract data according to Toyota TPMS format
    instance->id = ((uint32_t)data[0] << 24) | 
                   ((uint32_t)data[1] << 16) | 
                   ((uint32_t)data[2] << 8) | 
                   data[3];
    
    uint8_t status = (data[4] & 0x80) | (data[6] & 0x7f);
    uint8_t pressure1 = ((data[4] & 0x7f) << 1) | (data[5] >> 7);
    uint8_t temp = ((data[5] & 0x7f) << 1) | (data[6] >> 7);
    uint8_t pressure2 = data[7] ^ 0xff;
    
    // Validate pressure redundancy
    if(pressure1 != pressure2) {
        FURI_LOG_W(TAG, "Pressure validation failed: %02x vs %02x", pressure1, pressure2);
        return;
    }
    
    // Convert to physical units
    instance->temperature = temp - 40.0f;  // Offset by 40°C
    instance->pressure = (pressure1 * 0.25f - 7.0f) * 0.0689476f;  // Convert PSI to bar
    instance->battery_low = (status & 0x80) ? 1 : 0;  // Status bit indicates battery
    
    // Store raw data
    instance->data = ((uint64_t)data[0] << 56) | 
                     ((uint64_t)data[1] << 48) |
                     ((uint64_t)data[2] << 40) |
                     ((uint64_t)data[3] << 32) |
                     ((uint64_t)data[4] << 24) |
                     ((uint64_t)data[5] << 16) |
                     ((uint64_t)data[6] << 8) |
                     data[7];
    instance->data_count_bit = 72;
}

void tpms_protocol_decoder_toyota_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    
    switch(instance->decoder.parser_step) {
        case ToyotaDecoderStepReset:
            // Look for sync pattern in the data stream
            if(level && DURATION_DIFF(duration, tpms_protocol_toyota_const.te_short) < 
               tpms_protocol_toyota_const.te_delta) {
                instance->decoder.parser_step = ToyotaDecoderStepSync;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->manchester_bit_count = 0;
            }
            break;
            
        case ToyotaDecoderStepSync:
            // Accumulate bits to find sync pattern
            if(DURATION_DIFF(duration, tpms_protocol_toyota_const.te_short) < 
               tpms_protocol_toyota_const.te_delta) {
                subghz_protocol_blocks_add_bit(&instance->decoder, level);
                
                if(instance->decoder.decode_count_bit >= 12) {
                    // Check for sync pattern (0xa9e0 >> 4 = 0xa9e)
                    uint16_t sync_check = (instance->decoder.decode_data >> 
                                         (instance->decoder.decode_count_bit - 12)) & 0xFFF;
                    if(sync_check == 0xa9e) {
                        FURI_LOG_D(TAG, "Toyota sync found");
                        instance->decoder.parser_step = ToyotaDecoderStepData;
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        instance->manchester_bit_count = 0;
                        memset(instance->manchester_data, 0, sizeof(instance->manchester_data));
                    }
                    
                    // Prevent overflow
                    if(instance->decoder.decode_count_bit > 24) {
                        instance->decoder.parser_step = ToyotaDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = ToyotaDecoderStepReset;
            }
            break;
            
        case ToyotaDecoderStepData:
            // Collect differential Manchester encoded data
            if(DURATION_DIFF(duration, tpms_protocol_toyota_const.te_short) < 
               tpms_protocol_toyota_const.te_delta) {
                
                // Simple differential Manchester decoding
                // This is a simplified version - full implementation would need proper
                // differential Manchester decoder similar to rtl_433
                subghz_protocol_blocks_add_bit(&instance->decoder, level);
                
                if(instance->decoder.decode_count_bit >= 
                   tpms_protocol_toyota_const.min_count_bit_for_found) {
                    
                    // Convert to byte array for processing
                    for(int i = 0; i < 9; i++) {
                        instance->manchester_data[i] = 
                            (instance->decoder.decode_data >> ((8-i) * 8)) & 0xFF;
                    }
                    
                    FURI_LOG_D(TAG, "Toyota data: %02x%02x%02x%02x%02x%02x%02x%02x%02x",
                        instance->manchester_data[0], instance->manchester_data[1],
                        instance->manchester_data[2], instance->manchester_data[3],
                        instance->manchester_data[4], instance->manchester_data[5],
                        instance->manchester_data[6], instance->manchester_data[7],
                        instance->manchester_data[8]);
                    
                    if(tpms_protocol_toyota_check_crc(instance->manchester_data)) {
                        tpms_protocol_toyota_analyze(&instance->generic, instance->manchester_data);
                        
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    } else {
                        FURI_LOG_D(TAG, "Toyota CRC check failed");
                    }
                    
                    instance->decoder.parser_step = ToyotaDecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = ToyotaDecoderStepReset;
            }
            break;
    }
}

uint8_t tpms_protocol_decoder_toyota_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_toyota_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_toyota_deserialize(
    void* context, 
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_toyota_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_toyota_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%s\r\n"
        "Temp:%.1f C Bar:%.2f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low == TPMS_NO_BATT ? "?" : 
            (instance->generic.battery_low ? "LOW" : "OK"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
