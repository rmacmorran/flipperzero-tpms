#include "ford_tpms.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "Ford TPMS"

/**
 * Ford TPMS Protocol
 * 
 * Based on rtl_433 implementation:
 * - FSK modulation with Manchester encoding
 * - 315MHz or 433MHz frequency
 * - 8-byte packet structure (64 bits)
 * - Simple checksum validation
 * 
 * Packet format:
 * - Bytes 0-3: 32-bit sensor ID
 * - Byte 4: Pressure bits
 * - Byte 5: Temperature (with validity flag)
 * - Byte 6: Flags (moving, learn, pressure MSB)
 * - Byte 7: Checksum (sum of bytes 0-6)
 * 
 * Features:
 * - Temperature: offset by +56°C (when valid)
 * - Pressure: in PSI * 4 with 9th bit in flags
 * - Moving/Learn/Rest modes
 * - Battery and status flags
 */

// Sync pattern: 0xaa, 0xa9 (16 bits, inverted from 55 55 55 56)
#define FORD_SYNC_PATTERN_1 0xaa
#define FORD_SYNC_PATTERN_2 0xa9

static const SubGhzBlockConst tpms_protocol_ford_const = {
    .te_short = 52,     // FSK bit period ~52us
    .te_long = 52,      // Same for FSK
    .te_delta = 15,     // Tolerance
    .min_count_bit_for_found = 64,  // 8 bytes * 8 bits
};

struct TPMSProtocolDecoderFord {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    
    uint8_t manchester_data[8];  // Buffer for Manchester decoded data
    ManchesterState manchester_state;
};

struct TPMSProtocolEncoderFord {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    FordDecoderStepReset = 0,
    FordDecoderStepSync,
    FordDecoderStepData,
} FordDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_ford_decoder = {
    .alloc = tpms_protocol_decoder_ford_alloc,
    .free = tpms_protocol_decoder_ford_free,
    .feed = tpms_protocol_decoder_ford_feed,
    .reset = tpms_protocol_decoder_ford_reset,
    .get_hash_data = tpms_protocol_decoder_ford_get_hash_data,
    .serialize = tpms_protocol_decoder_ford_serialize,
    .deserialize = tpms_protocol_decoder_ford_deserialize,
    .get_string = tpms_protocol_decoder_ford_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_ford_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_ford = {
    .name = TPMS_PROTOCOL_FORD_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &tpms_protocol_ford_decoder,
    .encoder = &tpms_protocol_ford_encoder,
};

void* tpms_protocol_decoder_ford_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderFord* instance = malloc(sizeof(TPMSProtocolDecoderFord));
    instance->base.protocol = &tpms_protocol_ford;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_ford_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    free(instance);
}

void tpms_protocol_decoder_ford_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    instance->decoder.parser_step = FordDecoderStepReset;
    instance->manchester_state = ManchesterStateStart1;
}

static bool tpms_protocol_ford_check_checksum(uint8_t* data) {
    // Simple sum checksum: sum of bytes 0-6 should equal byte 7
    uint8_t checksum = 0;
    for(int i = 0; i < 7; i++) {
        checksum += data[i];
    }
    return (checksum & 0xff) == data[7];
}

static void tpms_protocol_ford_analyze(TPMSBlockGeneric* instance, uint8_t* data) {
    // Extract data according to Ford TPMS format
    instance->id = ((uint32_t)data[0] << 24) | 
                   ((uint32_t)data[1] << 16) | 
                   ((uint32_t)data[2] << 8) | 
                   data[3];
    
    // Pressure calculation: PSI * 4 with 9th bit in flags
    uint16_t pressure_raw = data[4];
    if(data[6] & 0x20) { // 9th bit of pressure
        pressure_raw |= 0x100;
    }
    float pressure_psi = pressure_raw * 0.25f;
    instance->pressure = pressure_psi * 0.0689476f; // Convert PSI to bar
    
    // Temperature calculation (when valid)
    bool temperature_valid = !(data[5] & 0x80);
    if(temperature_valid) {
        instance->temperature = (data[5] & 0x7f) - 56.0f; // Offset by 56°C
    } else {
        instance->temperature = -1000.0f; // Invalid temperature marker
    }
    
    // Status flags analysis
    uint8_t flags = data[6];
    bool moving = (flags & 0x44) == 0x44;    // Moving mode
    bool learn = (flags & 0x08) == 0x08;     // Learn mode
    bool rest = (flags & 0x4c) == 0x04;      // At rest mode
    
    // Store additional info in battery_low field (repurposed for Ford-specific data)
    instance->battery_low = flags; // Store all flags for analysis
    
    // Store raw data
    instance->data = ((uint64_t)data[0] << 56) | 
                     ((uint64_t)data[1] << 48) |
                     ((uint64_t)data[2] << 40) |
                     ((uint64_t)data[3] << 32) |
                     ((uint64_t)data[4] << 24) |
                     ((uint64_t)data[5] << 16) |
                     ((uint64_t)data[6] << 8) |
                     data[7];
    instance->data_count_bit = 64;
    
    FURI_LOG_I(TAG, "Ford TPMS: ID=%08lX P=%.1f PSI T=%.0f°C Moving=%d Learn=%d", 
        instance->id, pressure_psi, (double)instance->temperature, moving, learn);
}

void tpms_protocol_decoder_ford_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    
    switch(instance->decoder.parser_step) {
        case FordDecoderStepReset:
            // Look for sync pattern in the data stream
            if(level && DURATION_DIFF(duration, tpms_protocol_ford_const.te_short) < 
               tpms_protocol_ford_const.te_delta) {
                instance->decoder.parser_step = FordDecoderStepSync;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->manchester_state = ManchesterStateStart1;
            }
            break;
            
        case FordDecoderStepSync:
            // Accumulate bits to find sync pattern (0xaaa9)
            if(DURATION_DIFF(duration, tpms_protocol_ford_const.te_short) < 
               tpms_protocol_ford_const.te_delta) {
                subghz_protocol_blocks_add_bit(&instance->decoder, level);
                
                if(instance->decoder.decode_count_bit >= 16) {
                    // Check for sync pattern
                    uint16_t sync_check = (instance->decoder.decode_data >> 
                                         (instance->decoder.decode_count_bit - 16)) & 0xFFFF;
                    if(sync_check == 0xaaa9) {
                        FURI_LOG_D(TAG, "Ford sync found");
                        instance->decoder.parser_step = FordDecoderStepData;
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        instance->manchester_state = ManchesterStateStart1;
                        memset(instance->manchester_data, 0, sizeof(instance->manchester_data));
                    }
                    
                    // Prevent overflow
                    if(instance->decoder.decode_count_bit > 32) {
                        instance->decoder.parser_step = FordDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = FordDecoderStepReset;
            }
            break;
            
        case FordDecoderStepData:
            // Collect Manchester encoded data
            if(DURATION_DIFF(duration, tpms_protocol_ford_const.te_short) < 
               tpms_protocol_ford_const.te_delta) {
                
                // Use simplified Manchester decoding similar to other protocols
                bool bit = false;
                ManchesterEvent event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                
                if(manchester_advance(instance->manchester_state, event, 
                                    &instance->manchester_state, &bit)) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, bit);
                    
                    if(instance->decoder.decode_count_bit >= 
                       tpms_protocol_ford_const.min_count_bit_for_found) {
                        
                        // Convert to byte array for processing
                        for(int i = 0; i < 8; i++) {
                            instance->manchester_data[i] = 
                                (instance->decoder.decode_data >> ((7-i) * 8)) & 0xFF;
                        }
                        
                        FURI_LOG_D(TAG, "Ford data: %02x%02x%02x%02x%02x%02x%02x%02x",
                            instance->manchester_data[0], instance->manchester_data[1],
                            instance->manchester_data[2], instance->manchester_data[3],
                            instance->manchester_data[4], instance->manchester_data[5],
                            instance->manchester_data[6], instance->manchester_data[7]);
                        
                        if(tpms_protocol_ford_check_checksum(instance->manchester_data)) {
                            tpms_protocol_ford_analyze(&instance->generic, instance->manchester_data);
                            
                            if(instance->base.callback) {
                                instance->base.callback(&instance->base, instance->base.context);
                            }
                        } else {
                            FURI_LOG_D(TAG, "Ford checksum failed");
                        }
                        
                        instance->decoder.parser_step = FordDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = FordDecoderStepReset;
            }
            break;
    }
}

uint8_t tpms_protocol_decoder_ford_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_ford_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_ford_deserialize(
    void* context, 
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_ford_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_ford_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    
    // Calculate pressure in PSI from stored data
    uint8_t flags = instance->generic.battery_low; // Repurposed field
    uint16_t pressure_raw = (instance->generic.data >> 32) & 0xFF;
    if(flags & 0x20) pressure_raw |= 0x100;
    float pressure_psi = pressure_raw * 0.25f;
    
    // Determine mode
    const char* mode = "Rest";
    if((flags & 0x44) == 0x44) mode = "Moving";
    else if(flags & 0x08) mode = "Learn";
    
    bool temp_valid = instance->generic.temperature > -999.0f;
    
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Mode:%s\r\n"
        "Pressure:%.1f PSI\r\n"
        "%s%.0f C",
        instance->generic.protocol_name,
        instance->generic.id,
        mode,
        (double)pressure_psi,
        temp_valid ? "Temp:" : "Temp:N/A",
        temp_valid ? (double)instance->generic.temperature : 0.0);
}
