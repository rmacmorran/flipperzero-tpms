#include "nissan_tpms.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "Nissan TPMS"

/**
 * Nissan TPMS Protocol
 * 
 * Based on rtl_433 implementation:
 * - ASK/OOK modulation with PWM encoding
 * - 433MHz frequency
 * - 9-byte packet structure (72 bits)
 * - CRC-8 validation (poly 0x07)
 * 
 * Packet format:
 * - Preamble: 0xAAAAA (20 bits)
 * - Sync: 0x5A (8 bits)
 * - ID: 32 bits (4 bytes)
 * - Pressure: 16 bits (big endian, multiply by 0.25 for kPa)
 * - Temperature: 8 bits (°C + 40)
 * - Flags: 8 bits (battery, learn mode, etc.)
 * - CRC: 8 bits (CRC-8 with poly 0x07)
 * 
 * Features:
 * - PWM encoding (short/long pulses)
 * - Temperature: offset by +40°C
 * - Pressure: multiply by 0.25 for kPa
 * - Battery and learn mode flags
 */

// Nissan sync pattern: 0x5A after preamble
#define NISSAN_SYNC_PATTERN 0x5A
#define NISSAN_PREAMBLE 0xAAAAA

static const SubGhzBlockConst tpms_protocol_nissan_const = {
    .te_short = 52,     // Short pulse ~52us
    .te_long = 104,     // Long pulse ~104us (2x short)
    .te_delta = 15,     // Tolerance
    .min_count_bit_for_found = 72,  // 9 bytes * 8 bits
};

struct TPMSProtocolDecoderNissan {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    
    uint8_t pwm_data[9];  // Buffer for PWM decoded data
    bool last_level;
    uint32_t last_duration;
};

struct TPMSProtocolEncoderNissan {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    NissanDecoderStepReset = 0,
    NissanDecoderStepPreamble,
    NissanDecoderStepData,
} NissanDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_nissan_decoder = {
    .alloc = tpms_protocol_decoder_nissan_alloc,
    .free = tpms_protocol_decoder_nissan_free,
    .feed = tpms_protocol_decoder_nissan_feed,
    .reset = tpms_protocol_decoder_nissan_reset,
    .get_hash_data = tpms_protocol_decoder_nissan_get_hash_data,
    .serialize = tpms_protocol_decoder_nissan_serialize,
    .deserialize = tpms_protocol_decoder_nissan_deserialize,
    .get_string = tpms_protocol_decoder_nissan_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_nissan_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_nissan = {
    .name = TPMS_PROTOCOL_NISSAN_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable,
    .decoder = &tpms_protocol_nissan_decoder,
    .encoder = &tpms_protocol_nissan_encoder,
};

// CRC-8 with polynomial 0x07
static uint8_t nissan_crc8(uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            if(crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void* tpms_protocol_decoder_nissan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderNissan* instance = malloc(sizeof(TPMSProtocolDecoderNissan));
    instance->base.protocol = &tpms_protocol_nissan;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_nissan_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    free(instance);
}

void tpms_protocol_decoder_nissan_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    instance->decoder.parser_step = NissanDecoderStepReset;
    instance->last_level = false;
    instance->last_duration = 0;
}

static bool tpms_protocol_nissan_check_crc(uint8_t* data) {
    // CRC covers bytes 0-7, check against byte 8
    uint8_t calc_crc = nissan_crc8(data, 8);
    return calc_crc == data[8];
}

static void tpms_protocol_nissan_analyze(TPMSBlockGeneric* instance, uint8_t* data) {
    // Extract data according to Nissan TPMS format
    // Skip first byte (preamble/sync), data starts at byte 1
    instance->id = ((uint32_t)data[1] << 24) | 
                   ((uint32_t)data[2] << 16) | 
                   ((uint32_t)data[3] << 8) | 
                   data[4];
    
    // Pressure calculation: 16-bit big endian, multiply by 0.25 for kPa
    uint16_t pressure_raw = ((uint16_t)data[5] << 8) | data[6];
    float pressure_kpa = pressure_raw * 0.25f;
    instance->pressure = pressure_kpa * 0.01f; // Convert kPa to bar
    
    // Temperature calculation: °C + 40 offset
    uint8_t temp_raw = data[7];
    instance->temperature = temp_raw - 40.0f;
    
    // Status analysis from flags
    uint8_t flags = data[8];
    bool battery_low = (flags & 0x80) == 0x80;  // Battery low flag
    bool learn_mode = (flags & 0x40) == 0x40;   // Learn mode flag
    
    instance->battery_low = battery_low;
    
    // Store raw data (all 9 bytes)
    instance->data = 0;
    for(int i = 0; i < 9; i++) {
        instance->data |= ((uint64_t)data[i] << ((8-i) * 8));
    }
    instance->data_count_bit = 72;
    
    FURI_LOG_I(TAG, "Nissan TPMS: ID=%08lX P=%.1f kPa T=%.0f°C Learn=%d Batt_Low=%d", 
        instance->id, (double)pressure_kpa, (double)instance->temperature, learn_mode, battery_low);
}

void tpms_protocol_decoder_nissan_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    
    switch(instance->decoder.parser_step) {
        case NissanDecoderStepReset:
            // Look for start of preamble (long high pulse)
            if(level && duration >= (tpms_protocol_nissan_const.te_long - 
                                   tpms_protocol_nissan_const.te_delta)) {
                instance->decoder.parser_step = NissanDecoderStepPreamble;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            }
            break;
            
        case NissanDecoderStepPreamble:
            // Decode PWM bits and look for sync pattern
            if(!level) { // Process on falling edge
                bool bit_value;
                
                // Decode PWM: short high = 0, long high = 1
                if(instance->last_duration >= (tpms_protocol_nissan_const.te_long - 
                                             tpms_protocol_nissan_const.te_delta)) {
                    bit_value = 1;
                } else if(instance->last_duration >= (tpms_protocol_nissan_const.te_short - 
                                                    tpms_protocol_nissan_const.te_delta)) {
                    bit_value = 0;
                } else {
                    instance->decoder.parser_step = NissanDecoderStepReset;
                    break;
                }
                
                subghz_protocol_blocks_add_bit(&instance->decoder, bit_value);
                
                // Look for sync pattern (0x5A) after preamble
                if(instance->decoder.decode_count_bit >= 28) {  // 20-bit preamble + 8-bit sync
                    uint8_t sync_check = instance->decoder.decode_data & 0xFF;
                    if(sync_check == NISSAN_SYNC_PATTERN) {
                        FURI_LOG_D(TAG, "Nissan sync found");
                        instance->decoder.parser_step = NissanDecoderStepData;
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        memset(instance->pwm_data, 0, sizeof(instance->pwm_data));
                    }
                    
                    // Prevent overflow
                    if(instance->decoder.decode_count_bit > 40) {
                        instance->decoder.parser_step = NissanDecoderStepReset;
                    }
                }
            }
            instance->last_level = level;
            instance->last_duration = duration;
            break;
            
        case NissanDecoderStepData:
            // Continue PWM decoding for data payload
            if(!level) { // Process on falling edge
                bool bit_value;
                
                // Decode PWM: short high = 0, long high = 1
                if(instance->last_duration >= (tpms_protocol_nissan_const.te_long - 
                                             tpms_protocol_nissan_const.te_delta)) {
                    bit_value = 1;
                } else if(instance->last_duration >= (tpms_protocol_nissan_const.te_short - 
                                                    tpms_protocol_nissan_const.te_delta)) {
                    bit_value = 0;
                } else {
                    instance->decoder.parser_step = NissanDecoderStepReset;
                    break;
                }
                
                subghz_protocol_blocks_add_bit(&instance->decoder, bit_value);
                
                if(instance->decoder.decode_count_bit >= 
                   tpms_protocol_nissan_const.min_count_bit_for_found) {
                    
                    // Convert to byte array for processing
                    instance->pwm_data[0] = NISSAN_SYNC_PATTERN; // Sync byte
                    for(int i = 0; i < 8; i++) {
                        instance->pwm_data[i + 1] = 
                            (instance->decoder.decode_data >> ((7-i) * 8)) & 0xFF;
                    }
                    
                    FURI_LOG_D(TAG, "Nissan data: %02x%02x%02x%02x%02x%02x%02x%02x%02x",
                        instance->pwm_data[0], instance->pwm_data[1],
                        instance->pwm_data[2], instance->pwm_data[3],
                        instance->pwm_data[4], instance->pwm_data[5],
                        instance->pwm_data[6], instance->pwm_data[7],
                        instance->pwm_data[8]);
                    
                    if(tpms_protocol_nissan_check_crc(instance->pwm_data)) {
                        tpms_protocol_nissan_analyze(&instance->generic, instance->pwm_data);
                        
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    } else {
                        FURI_LOG_D(TAG, "Nissan CRC failed");
                    }
                    
                    instance->decoder.parser_step = NissanDecoderStepReset;
                }
            }
            instance->last_level = level;
            instance->last_duration = duration;
            break;
    }
}

uint8_t tpms_protocol_decoder_nissan_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_nissan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_nissan_deserialize(
    void* context, 
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_nissan_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_nissan_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    
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
