#include "gm_tpms.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "GM TPMS"

/**
 * GM TPMS Protocol
 * 
 * Based on rtl_433 implementation:
 * - FSK modulation with Manchester encoding
 * - 315MHz frequency
 * - 9-byte packet structure (72 bits)
 * - CRC-8 validation
 * 
 * Packet format:
 * - Preamble: 0x555 (12 bits)
 * - Sync: 0xD (4 bits) = 1101b
 * - ID: 32 bits (4 bytes)
 * - Status/Flags: 8 bits
 * - Pressure: 8 bits (kPa + 50)
 * - Temperature: 8 bits (°C + 40)
 * - CRC: 8 bits (Dallas/Maxim CRC-8)
 * 
 * Features:
 * - Temperature: offset by +40°C
 * - Pressure: kPa + 50 offset
 * - Fast/Slow transmit modes
 * - Battery and status flags
 */

// GM sync pattern after preamble: 0xD (4 bits)
#define GM_SYNC_PATTERN 0xD
#define GM_PREAMBLE 0x555

static const SubGhzBlockConst tpms_protocol_gm_const = {
    .te_short = 100,    // Manchester bit period ~100us
    .te_long = 100,     // Same for FSK
    .te_delta = 20,     // Tolerance
    .min_count_bit_for_found = 72,  // 9 bytes * 8 bits
};

struct TPMSProtocolDecoderGM {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    
    uint8_t manchester_data[9];  // Buffer for Manchester decoded data
    ManchesterState manchester_state;
};

struct TPMSProtocolEncoderGM {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    GMDecoderStepReset = 0,
    GMDecoderStepPreamble,
    GMDecoderStepSync,
    GMDecoderStepData,
} GMDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_gm_decoder = {
    .alloc = tpms_protocol_decoder_gm_alloc,
    .free = tpms_protocol_decoder_gm_free,
    .feed = tpms_protocol_decoder_gm_feed,
    .reset = tpms_protocol_decoder_gm_reset,
    .get_hash_data = tpms_protocol_decoder_gm_get_hash_data,
    .serialize = tpms_protocol_decoder_gm_serialize,
    .deserialize = tpms_protocol_decoder_gm_deserialize,
    .get_string = tpms_protocol_decoder_gm_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_gm_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_gm = {
    .name = TPMS_PROTOCOL_GM_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &tpms_protocol_gm_decoder,
    .encoder = &tpms_protocol_gm_encoder,
};

// CRC-8 Dallas/Maxim calculation
static uint8_t gm_crc8(uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            if(crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void* tpms_protocol_decoder_gm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderGM* instance = malloc(sizeof(TPMSProtocolDecoderGM));
    instance->base.protocol = &tpms_protocol_gm;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_gm_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    free(instance);
}

void tpms_protocol_decoder_gm_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    instance->decoder.parser_step = GMDecoderStepReset;
    instance->manchester_state = ManchesterStateStart1;
}

static bool tpms_protocol_gm_check_crc(uint8_t* data) {
    // CRC covers bytes 0-7, check against byte 8
    uint8_t calc_crc = gm_crc8(data, 8);
    return calc_crc == data[8];
}

static void tpms_protocol_gm_analyze(TPMSBlockGeneric* instance, uint8_t* data) {
    // Extract data according to GM TPMS format
    // Skip first 2 bytes (preamble/sync), data starts at byte 2
    instance->id = ((uint32_t)data[2] << 24) | 
                   ((uint32_t)data[3] << 16) | 
                   ((uint32_t)data[4] << 8) | 
                   data[5];
    
    // Status/flags byte
    uint8_t status = data[6];
    
    // Pressure calculation: kPa + 50 offset
    uint8_t pressure_raw = data[7];
    float pressure_kpa = pressure_raw - 50.0f;
    instance->pressure = pressure_kpa * 0.01f; // Convert kPa to bar
    
    // Temperature calculation: °C + 40 offset
    uint8_t temp_raw = data[8];
    instance->temperature = temp_raw - 40.0f;
    
    // Status analysis from flags
    bool fast_mode = (status & 0x80) == 0x80;  // Fast transmit mode
    bool battery_low = (status & 0x40) == 0x40; // Battery low
    
    instance->battery_low = battery_low;
    
    // Store raw data (all 9 bytes)
    instance->data = 0;
    for(int i = 0; i < 9; i++) {
        instance->data |= ((uint64_t)data[i] << ((8-i) * 8));
    }
    instance->data_count_bit = 72;
    
    FURI_LOG_I(TAG, "GM TPMS: ID=%08lX P=%.1f kPa T=%.0f°C Fast=%d Batt_Low=%d", 
        instance->id, (double)pressure_kpa, (double)instance->temperature, fast_mode, battery_low);
}

void tpms_protocol_decoder_gm_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    
    switch(instance->decoder.parser_step) {
        case GMDecoderStepReset:
            // Look for preamble (alternating pattern)
            if(level && DURATION_DIFF(duration, tpms_protocol_gm_const.te_short) < 
               tpms_protocol_gm_const.te_delta) {
                instance->decoder.parser_step = GMDecoderStepPreamble;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->manchester_state = ManchesterStateStart1;
            }
            break;
            
        case GMDecoderStepPreamble:
            // Look for preamble and sync pattern
            if(DURATION_DIFF(duration, tpms_protocol_gm_const.te_short) < 
               tpms_protocol_gm_const.te_delta) {
                subghz_protocol_blocks_add_bit(&instance->decoder, level);
                
                if(instance->decoder.decode_count_bit >= 16) {
                    // Look for preamble + sync: 555D (hex)
                    uint16_t pattern = (instance->decoder.decode_data >> 
                                      (instance->decoder.decode_count_bit - 16)) & 0xFFFF;
                    if(pattern == 0x555D) {
                        FURI_LOG_D(TAG, "GM preamble+sync found");
                        instance->decoder.parser_step = GMDecoderStepData;
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        instance->manchester_state = ManchesterStateStart1;
                        memset(instance->manchester_data, 0, sizeof(instance->manchester_data));
                    }
                    
                    // Prevent overflow
                    if(instance->decoder.decode_count_bit > 32) {
                        instance->decoder.parser_step = GMDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = GMDecoderStepReset;
            }
            break;
            
        case GMDecoderStepData:
            // Collect Manchester encoded data
            if(DURATION_DIFF(duration, tpms_protocol_gm_const.te_short) < 
               tpms_protocol_gm_const.te_delta) {
                
                // Use simplified Manchester decoding
                bool bit = false;
                ManchesterEvent event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                
                if(manchester_advance(instance->manchester_state, event, 
                                    &instance->manchester_state, &bit)) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, bit);
                    
                    if(instance->decoder.decode_count_bit >= 
                       tpms_protocol_gm_const.min_count_bit_for_found) {
                        
                        // Convert to byte array for processing
                        // Include preamble and sync for complete packet
                        instance->manchester_data[0] = 0x55; // Preamble high
                        instance->manchester_data[1] = 0x5D; // Preamble low + sync
                        for(int i = 0; i < 7; i++) {
                            instance->manchester_data[i + 2] = 
                                (instance->decoder.decode_data >> ((6-i) * 8)) & 0xFF;
                        }
                        
                        FURI_LOG_D(TAG, "GM data: %02x%02x%02x%02x%02x%02x%02x%02x%02x",
                            instance->manchester_data[0], instance->manchester_data[1],
                            instance->manchester_data[2], instance->manchester_data[3],
                            instance->manchester_data[4], instance->manchester_data[5],
                            instance->manchester_data[6], instance->manchester_data[7],
                            instance->manchester_data[8]);
                        
                        if(tpms_protocol_gm_check_crc(instance->manchester_data)) {
                            tpms_protocol_gm_analyze(&instance->generic, instance->manchester_data);
                            
                            if(instance->base.callback) {
                                instance->base.callback(&instance->base, instance->base.context);
                            }
                        } else {
                            FURI_LOG_D(TAG, "GM CRC failed");
                        }
                        
                        instance->decoder.parser_step = GMDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = GMDecoderStepReset;
            }
            break;
    }
}

uint8_t tpms_protocol_decoder_gm_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_gm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_gm_deserialize(
    void* context, 
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_gm_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_gm_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderGM* instance = context;
    
    // Calculate pressure in kPa from stored data
    float pressure_kpa = instance->generic.pressure * 100.0f; // Convert bar back to kPa
    
    // Determine mode from status flags (using battery_low field to store status)
    const char* mode = "Unknown";
    if(instance->generic.battery_low) {
        mode = "Battery Low";
    } else {
        mode = "Normal";
    }
    
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Mode:%s\r\n"
        "Pressure:%.1f kPa\r\n"
        "Temp:%.0f C",
        instance->generic.protocol_name,
        instance->generic.id,
        mode,
        (double)pressure_kpa,
        (double)instance->generic.temperature);
}
