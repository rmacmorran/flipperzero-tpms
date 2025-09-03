#include "hyundai_tpms.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "Hyundai TPMS"

/**
 * Hyundai TPMS Protocol (VDO variant)
 * 
 * Based on rtl_433 implementation:
 * - FSK modulation with Manchester encoding  
 * - 433MHz frequency
 * - 10-byte packet structure (80 bits)
 * - CRC-8 validation
 * 
 * Packet format:
 * - Preamble: 0x55555 (20 bits)
 * - Sync: 0x56 (8 bits) 
 * - ID: 32 bits (4 bytes)
 * - Status: 8 bits
 * - Pressure: 8 bits (kPa + 40)
 * - Temperature: 8 bits (°C + 50)
 * - Flags: 8 bits (battery, fast/slow mode)
 * - CRC: 8 bits
 * 
 * Features:
 * - Temperature: offset by +50°C
 * - Pressure: kPa + 40 offset
 * - Fast/Slow transmit modes
 * - Battery status monitoring
 */

// Hyundai sync pattern: 0x56 after preamble
#define HYUNDAI_SYNC_PATTERN 0x56
#define HYUNDAI_PREAMBLE 0x55555

static const SubGhzBlockConst tpms_protocol_hyundai_const = {
    .te_short = 50,     // Manchester bit period ~50us
    .te_long = 50,      // Same for FSK
    .te_delta = 15,     // Tolerance
    .min_count_bit_for_found = 80,  // 10 bytes * 8 bits
};

struct TPMSProtocolDecoderHyundai {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    
    uint8_t manchester_data[10];  // Buffer for Manchester decoded data
    ManchesterState manchester_state;
};

struct TPMSProtocolEncoderHyundai {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    HyundaiDecoderStepReset = 0,
    HyundaiDecoderStepPreamble,
    HyundaiDecoderStepData,
} HyundaiDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_hyundai_decoder = {
    .alloc = tpms_protocol_decoder_hyundai_alloc,
    .free = tpms_protocol_decoder_hyundai_free,
    .feed = tpms_protocol_decoder_hyundai_feed,
    .reset = tpms_protocol_decoder_hyundai_reset,
    .get_hash_data = tpms_protocol_decoder_hyundai_get_hash_data,
    .serialize = tpms_protocol_decoder_hyundai_serialize,
    .deserialize = tpms_protocol_decoder_hyundai_deserialize,
    .get_string = tpms_protocol_decoder_hyundai_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_hyundai_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_hyundai = {
    .name = TPMS_PROTOCOL_HYUNDAI_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &tpms_protocol_hyundai_decoder,
    .encoder = &tpms_protocol_hyundai_encoder,
};

// CRC-8 calculation (standard poly 0x31)
static uint8_t hyundai_crc8(uint8_t* data, size_t len) {
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

void* tpms_protocol_decoder_hyundai_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHyundai* instance = malloc(sizeof(TPMSProtocolDecoderHyundai));
    instance->base.protocol = &tpms_protocol_hyundai;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_hyundai_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    free(instance);
}

void tpms_protocol_decoder_hyundai_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    instance->decoder.parser_step = HyundaiDecoderStepReset;
    instance->manchester_state = ManchesterStateStart1;
}

static bool tpms_protocol_hyundai_check_crc(uint8_t* data) {
    // CRC covers bytes 0-8, check against byte 9
    uint8_t calc_crc = hyundai_crc8(data, 9);
    return calc_crc == data[9];
}

static void tpms_protocol_hyundai_analyze(TPMSBlockGeneric* instance, uint8_t* data) {
    // Extract data according to Hyundai TPMS format
    // Skip first 3 bytes (preamble/sync), data starts at byte 3
    instance->id = ((uint32_t)data[3] << 24) | 
                   ((uint32_t)data[4] << 16) | 
                   ((uint32_t)data[5] << 8) | 
                   data[6];
    
    // Status byte
    uint8_t status = data[7];
    
    // Pressure calculation: kPa + 40 offset
    uint8_t pressure_raw = data[8];
    float pressure_kpa = pressure_raw - 40.0f;
    if(pressure_kpa < 0) pressure_kpa = 0; // Sanity check
    instance->pressure = pressure_kpa * 0.01f; // Convert kPa to bar
    
    // Temperature calculation: °C + 50 offset
    uint8_t temp_raw = data[9];
    instance->temperature = temp_raw - 50.0f;
    
    // Status analysis from flags
    bool fast_mode = (status & 0x80) == 0x80;    // Fast transmit mode
    bool battery_low = (status & 0x40) == 0x40;  // Battery low
    bool learn_mode = (status & 0x20) == 0x20;   // Learn mode
    
    instance->battery_low = battery_low;
    
    // Store raw data (all 10 bytes)
    instance->data = 0;
    for(int i = 0; i < 10; i++) {
        if(i < 8) {
            instance->data |= ((uint64_t)data[i] << ((7-i) * 8));
        }
    }
    instance->data_count_bit = 80;
    
    FURI_LOG_I(TAG, "Hyundai TPMS: ID=%08lX P=%.1f kPa T=%.0f°C Fast=%d Learn=%d Batt_Low=%d", 
        instance->id, (double)pressure_kpa, (double)instance->temperature, 
        fast_mode, learn_mode, battery_low);
}

void tpms_protocol_decoder_hyundai_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    
    switch(instance->decoder.parser_step) {
        case HyundaiDecoderStepReset:
            // Look for preamble start
            if(level && DURATION_DIFF(duration, tpms_protocol_hyundai_const.te_short) < 
               tpms_protocol_hyundai_const.te_delta) {
                instance->decoder.parser_step = HyundaiDecoderStepPreamble;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->manchester_state = ManchesterStateStart1;
            }
            break;
            
        case HyundaiDecoderStepPreamble:
            // Look for preamble and sync pattern
            if(DURATION_DIFF(duration, tpms_protocol_hyundai_const.te_short) < 
               tpms_protocol_hyundai_const.te_delta) {
                subghz_protocol_blocks_add_bit(&instance->decoder, level);
                
                if(instance->decoder.decode_count_bit >= 28) {  // 20-bit preamble + 8-bit sync
                    // Look for preamble + sync: 5555556 (hex) - last 28 bits
                    uint32_t pattern = instance->decoder.decode_data & 0x0FFFFFFF;
                    if(pattern == 0x5555556) {
                        FURI_LOG_D(TAG, "Hyundai preamble+sync found");
                        instance->decoder.parser_step = HyundaiDecoderStepData;
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        instance->manchester_state = ManchesterStateStart1;
                        memset(instance->manchester_data, 0, sizeof(instance->manchester_data));
                    }
                    
                    // Prevent overflow
                    if(instance->decoder.decode_count_bit > 40) {
                        instance->decoder.parser_step = HyundaiDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = HyundaiDecoderStepReset;
            }
            break;
            
        case HyundaiDecoderStepData:
            // Collect Manchester encoded data
            if(DURATION_DIFF(duration, tpms_protocol_hyundai_const.te_short) < 
               tpms_protocol_hyundai_const.te_delta) {
                
                // Use simplified Manchester decoding
                bool bit = false;
                ManchesterEvent event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                
                if(manchester_advance(instance->manchester_state, event, 
                                    &instance->manchester_state, &bit)) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, bit);
                    
                    if(instance->decoder.decode_count_bit >= 
                       tpms_protocol_hyundai_const.min_count_bit_for_found) {
                        
                        // Convert to byte array for processing
                        // Include preamble and sync for complete packet
                        instance->manchester_data[0] = 0x55; // Preamble byte 1
                        instance->manchester_data[1] = 0x55; // Preamble byte 2
                        instance->manchester_data[2] = 0x56; // Preamble low nibble + sync
                        for(int i = 0; i < 7; i++) {
                            instance->manchester_data[i + 3] = 
                                (instance->decoder.decode_data >> ((6-i) * 8)) & 0xFF;
                        }
                        
                        FURI_LOG_D(TAG, "Hyundai data: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                            instance->manchester_data[0], instance->manchester_data[1],
                            instance->manchester_data[2], instance->manchester_data[3],
                            instance->manchester_data[4], instance->manchester_data[5],
                            instance->manchester_data[6], instance->manchester_data[7],
                            instance->manchester_data[8], instance->manchester_data[9]);
                        
                        if(tpms_protocol_hyundai_check_crc(instance->manchester_data)) {
                            tpms_protocol_hyundai_analyze(&instance->generic, instance->manchester_data);
                            
                            if(instance->base.callback) {
                                instance->base.callback(&instance->base, instance->base.context);
                            }
                        } else {
                            FURI_LOG_D(TAG, "Hyundai CRC failed");
                        }
                        
                        instance->decoder.parser_step = HyundaiDecoderStepReset;
                    }
                }
            } else {
                instance->decoder.parser_step = HyundaiDecoderStepReset;
            }
            break;
    }
}

uint8_t tpms_protocol_decoder_hyundai_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_deserialize(
    void* context, 
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_hyundai_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_hyundai_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHyundai* instance = context;
    
    // Calculate pressure in kPa from stored data
    float pressure_kpa = instance->generic.pressure * 100.0f; // Convert bar back to kPa
    
    // Determine mode from flags
    const char* mode = "Normal";
    if(instance->generic.battery_low) {
        mode = "Battery Low";
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
