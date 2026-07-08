// main\audio_player.cpp
#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "AUDIO";

// Pinout mapping for Seeed XIAO ESP32S3 external I2S Amplifier
#define I2S_LRC_PIN  GPIO_NUM_7 // D8
#define I2S_BCLK_PIN GPIO_NUM_8 // D9
#define I2S_DIN_PIN  GPIO_NUM_9 // D10

// Pinout mapping for Seeed XIAO ESP32S3 Sense INTERNAL PDM Microphone
#define I2S_MIC_CLK_PIN GPIO_NUM_42
#define I2S_MIC_DAT_PIN GPIO_NUM_41

static i2s_chan_handle_t tx_chan;
static i2s_chan_handle_t rx_chan;
static int32_t current_volume = 50; // 0-100 Default volume
static QueueHandle_t audio_queue;
static volatile bool audio_stop_requested = false; // Flag to instantly break playback loops

// Access the embedded WAV file created by CMake
extern const uint8_t dog_barking_wav_start[] asm("_binary_dog_barking_wav_start");
extern const uint8_t dog_barking_wav_end[]   asm("_binary_dog_barking_wav_end");

static void load_audio_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i32(my_handle, "aud_vol", &current_volume);
        nvs_close(my_handle);
    }
    if (current_volume < 0) current_volume = 0;
    if (current_volume > 100) current_volume = 100;
}

static void audio_task(void *pvParameter) {
    char sound_req[32];
    
    while (1) {
        // Wait until a sound play request is received in the queue
        if (xQueueReceive(audio_queue, &sound_req, portMAX_DELAY)) {
            
            // -------------------------------------------------------------
            // RECORD 3 SECONDS FROM PDM MIC & PLAY IT BACK
            // -------------------------------------------------------------
            if (strcmp(sound_req, "record_3s") == 0) {
                ESP_LOGI(TAG, "Starting 3-second recording from internal PDM mic...");
                uint32_t sample_rate = 16000;
                uint16_t channels = 1;
                uint16_t bit_depth = 16;
                size_t data_size = sample_rate * (bit_depth / 8) * channels * 3; // 96,000 bytes
                
                uint8_t* record_buffer = NULL;
#ifdef CONFIG_SPIRAM
                record_buffer = (uint8_t*) heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
                if (!record_buffer) {
                    record_buffer = (uint8_t*) malloc(data_size);
                }
                
                if (record_buffer) {
                    // Drain old data to avoid playing back history buffer noise
                    i2s_channel_disable(rx_chan);
                    i2s_channel_enable(rx_chan);
                    
                    ESP_LOGI(TAG, "Recording...");
                    audio_stop_requested = false;
                    
                    size_t chunk = sample_rate * (bit_depth / 8) * channels / 10; // 0.1 seconds per chunk
                    size_t total_read = 0;
                    
                    // Allow UI "Stop Sound" button to interrupt recording midway through
                    while (total_read < data_size && !audio_stop_requested) {
                        size_t to_read = (data_size - total_read > chunk) ? chunk : (data_size - total_read);
                        size_t bytes_read = 0;
                        i2s_channel_read(rx_chan, record_buffer + total_read, to_read, &bytes_read, portMAX_DELAY);
                        total_read += bytes_read;
                    }
                    
                    if (audio_stop_requested) {
                        ESP_LOGI(TAG, "Recording interrupted by STOP request.");
                    } else if (total_read > 0) {
                        ESP_LOGI(TAG, "Recording complete (%lu bytes). Playing back...", (unsigned long)total_read);
                        
                        i2s_channel_disable(tx_chan);
                        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
                        i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
                        i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO
                        );
                        i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);
                        i2s_channel_enable(tx_chan);
                        
                        int16_t* pcm_samples = (int16_t*)record_buffer;
                        size_t num_samples = total_read / 2;
                        
                        const float HARDWARE_POWER_LIMIT = 0.60f; 
                        const float MIC_GAIN = 4.0f; // Boost the raw mic capture via software gain multiplier
                        float vol_mult = ((float)current_volume / 100.0f) * HARDWARE_POWER_LIMIT * MIC_GAIN;
                        
                        const size_t CHUNK_SAMPLES = 512; 
                        int16_t out_buffer[CHUNK_SAMPLES];
                        size_t bytes_written;
                        
                        for (size_t i = 0; i < num_samples; i += CHUNK_SAMPLES) {
                            if (audio_stop_requested) break;
                            size_t chunk_size = (num_samples - i < CHUNK_SAMPLES) ? (num_samples - i) : CHUNK_SAMPLES;
                            
                            for (size_t j = 0; j < chunk_size; j++) {
                                if (current_volume == 0) {
                                    out_buffer[j] = 0;
                                } else {
                                    // Calculate boost and clamp within physical 16-bit limits
                                    int32_t val = (int32_t)(pcm_samples[i + j] * vol_mult);
                                    if (val > 32767) val = 32767;
                                    if (val < -32768) val = -32768;
                                    out_buffer[j] = (int16_t)val;
                                }
                            }
                            i2s_channel_write(tx_chan, out_buffer, chunk_size * 2, &bytes_written, portMAX_DELAY);
                        }
                        
                        if (audio_stop_requested) {
                            i2s_channel_disable(tx_chan);
                            i2s_channel_enable(tx_chan);
                            ESP_LOGI(TAG, "Playback interrupted by STOP request.");
                        } else {
                            i2s_channel_write(tx_chan, NULL, 0, &bytes_written, portMAX_DELAY);
                            ESP_LOGI(TAG, "Playback finished.");
                        }
                    }
                    free(record_buffer);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for recording.");
                }
                continue; // Skip the WAV file logic below
            }

            // -------------------------------------------------------------
            // PLAY STATIC WAV FILE
            // -------------------------------------------------------------
            const uint8_t* wav_start = NULL;
            const uint8_t* wav_end = NULL;
            
            if (strcmp(sound_req, "dog_bark") == 0) {
                wav_start = dog_barking_wav_start;
                wav_end = dog_barking_wav_end;
            }
            
            if (wav_start != NULL && wav_end > wav_start) {
                
                uint32_t sample_rate = 44100;
                uint16_t channels = 2;
                uint16_t bit_depth = 16;
                const uint8_t* audio_data = NULL;
                uint32_t data_size = 0;

                // Dynamic WAV RIFF Chunk Parser (Safely skips LIST/INFO metadata chunks)
                if (strncmp((const char*)wav_start, "RIFF", 4) == 0) {
                    size_t offset = 12; // Skip RIFF header, file size, and WAVE format
                    
                    while (offset < (size_t)(wav_end - wav_start) - 8) {
                        char chunk_id[5] = {0};
                        memcpy(chunk_id, wav_start + offset, 4);
                        uint32_t chunk_size = *(uint32_t*)(wav_start + offset + 4);
                        offset += 8;

                        if (strcmp(chunk_id, "fmt ") == 0) {
                            channels = *(uint16_t*)(wav_start + offset + 2);
                            sample_rate = *(uint32_t*)(wav_start + offset + 4);
                            bit_depth = *(uint16_t*)(wav_start + offset + 14);
                        } else if (strcmp(chunk_id, "data") == 0) {
                            audio_data = wav_start + offset;
                            data_size = chunk_size;
                            break; // We found the audio payload, stop parsing
                        }
                        offset += chunk_size; // Skip unknown chunks
                    }
                }

                if (audio_data != NULL && data_size > 0) {
                    ESP_LOGI(TAG, "Playing %s: %lu Hz, %d bit, %d ch | Size: %lu bytes", 
                             sound_req, sample_rate, bit_depth, channels, data_size);
                    
                    // Reconfigure I2S Clock to match the WAV file perfectly
                    i2s_channel_disable(tx_chan);
                    
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
                    i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
                    
                    // MAX98357A / HT517 requires standard PHILIPS formatting, NOT MSB/Left-Justified
                    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, 
                        channels == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO
                    );
                    i2s_channel_reconfig_std_slot(tx_chan, &slot_cfg);
                    
                    i2s_channel_enable(tx_chan);
                    
                    // Prevent reading past the embedded binary end if file is corrupted
                    if (audio_data + data_size > wav_end) {
                        data_size = wav_end - audio_data;
                    }
                    
                    int16_t* pcm_samples = (int16_t*)audio_data;
                    size_t num_samples = data_size / 2; // 16-bit integers
                    
                    // ---------------------------------------------------------
                    // HARDWARE SAFETY LIMITER FOR 1W 8-OHM SPEAKER
                    // ---------------------------------------------------------
                    const float HARDWARE_POWER_LIMIT = 0.60f; 
                    float vol_mult = ((float)current_volume / 100.0f) * HARDWARE_POWER_LIMIT;
                    
                    // Create a small buffer to scale volume in RAM without blocking
                    const size_t CHUNK_SAMPLES = 512; 
                    int16_t out_buffer[CHUNK_SAMPLES];
                    size_t bytes_written;
                    
                    audio_stop_requested = false; // Reset interrupt flag before playing
                    
                    for (size_t i = 0; i < num_samples; i += CHUNK_SAMPLES) {
                        
                        // Check if the user hit the STOP button
                        if (audio_stop_requested) {
                            break; // Immediately exit the audio blasting loop
                        }
                        
                        size_t chunk_size = (num_samples - i < CHUNK_SAMPLES) ? (num_samples - i) : CHUNK_SAMPLES;
                        
                        // Software Volume Application
                        for (size_t j = 0; j < chunk_size; j++) {
                            if (current_volume == 0) {
                                out_buffer[j] = 0; // Absolute Mute
                            } else {
                                out_buffer[j] = (int16_t)(pcm_samples[i + j] * vol_mult);
                            }
                        }
                        
                        esp_err_t err = i2s_channel_write(tx_chan, out_buffer, chunk_size * 2, &bytes_written, portMAX_DELAY);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "I2S Buffer Write Failed: %s", esp_err_to_name(err));
                            break; // Abort this track
                        }
                    }
                    
                    if (audio_stop_requested) {
                        // Rapidly clear the I2S hardware DMA buffer to stop the sound instantly
                        i2s_channel_disable(tx_chan);
                        i2s_channel_enable(tx_chan);
                        ESP_LOGI(TAG, "Audio playback interrupted by STOP request.");
                    } else {
                        // Flush the DMA buffers to ensure the sound finishes cleanly naturally
                        i2s_channel_write(tx_chan, NULL, 0, &bytes_written, portMAX_DELAY);
                        ESP_LOGI(TAG, "Playback finished.");
                    }
                    
                } else {
                    ESP_LOGE(TAG, "Invalid WAV structure: Could not find 'fmt ' or 'data' chunk.");
                }
            }
        }
    }
}

void audio_player_init() {
    load_audio_nvs();
    audio_queue = xQueueCreate(5, sizeof(char) * 32);

    // Initialize External I2S TX Amplifier
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // Initialize Internal PDM RX channel for Microphone
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan));

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000), // 16 kHz sample rate
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_MIC_CLK_PIN,
            .din = I2S_MIC_DAT_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    
    // Explicitly target left slot (usually tied to GND on breakouts and Sense shields)
    pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
    
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // Run audio handling strictly on Core 1 to avoid interrupting Servo PWM
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 4, NULL, 1);
    
    ESP_LOGI(TAG, "I2S Audio TX initialized on pins: BCLK=%d, LRC=%d, DIN=%d", I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);
    ESP_LOGI(TAG, "I2S PDM RX initialized on pins: CLK=%d, DAT=%d", I2S_MIC_CLK_PIN, I2S_MIC_DAT_PIN);
}

void audio_set_volume(int volume_pct) {
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    current_volume = (int32_t)volume_pct;
    
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "aud_vol", current_volume);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

int audio_get_volume() {
    return (int)current_volume;
}

void audio_play(const char* sound_name) {
    if (current_volume == 0) return; // Completely mute protection
    
    char req[32];
    strncpy(req, sound_name, sizeof(req) - 1);
    req[sizeof(req) - 1] = '\0';
    
    // Add to queue (non-blocking). If queue is full (spamming play), it ignores the request.
    xQueueSend(audio_queue, &req, 0); 
}

void audio_stop() {
    audio_stop_requested = true; // Signal the audio loop to instantly break
    xQueueReset(audio_queue);    // Clear any future queued sounds
}