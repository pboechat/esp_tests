#include "sdkconfig.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <cmath>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

#include <button_gpio.h>
#include <device.h>

#include <driver/gpio.h>
#include <driver/ledc.h>

#include <esp_event.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LED_GPIO_B GPIO_NUM_2
#define LED_GPIO_G GPIO_NUM_3
#define LED_GPIO_R GPIO_NUM_4

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_FREQUENCY  2000
#define LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define LEDC_DUTY_MAX   ((1 << LEDC_RESOLUTION) - 1)

#define MAX_BRIGHTNESS 254
#define MAX_HUE        254
#define MAX_SATURATION 254

/** Default attribute values used during initialization */
#define DEFAULT_POWER      true
#define DEFAULT_BRIGHTNESS 64
#define DEFAULT_X          32768
#define DEFAULT_Y          32768

// Color control feature map bits (Matter spec 1.3, Color Control cluster)
constexpr uint32_t kFeatureHueSaturation = (1u << 0);
constexpr uint32_t kFeatureEnhancedHue = (1u << 1);
constexpr uint32_t kFeatureXy = (1u << 3);
constexpr uint32_t kFeatureTemperature = (1u << 7);
constexpr uint32_t kAllColorFeatures = kFeatureHueSaturation | kFeatureEnhancedHue | kFeatureXy | kFeatureTemperature;
constexpr uint16_t kAllColorCapabilities = static_cast<uint16_t>(kAllColorFeatures);

constexpr uint32_t kColorFeatures = kAllColorFeatures;
constexpr uint16_t kColorCapabilities = kAllColorCapabilities;

#define ABORT_ON_FAILURE(x, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(unlikely(x)))                                                                                            \
        {                                                                                                              \
            __VA_ARGS__;                                                                                               \
            vTaskDelay(5000 / portTICK_PERIOD_MS);                                                                     \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

uint16_t g_light_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
constexpr const char *TAG = "led_strip";

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");
const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif

// Global variables
static uint16_t current_x = 0;
static uint16_t current_y = 0;
static uint8_t current_brightness = 0;
static bool current_power = false;
static uint8_t current_hue = 0;
static uint8_t current_saturation = 0;
static uint16_t current_temperature = 0;
static bool perform_factory_reset = false;

// Base color at full brightness (0.0f-1.0f per channel)
static float base_r = 1.0f;
static float base_g = 1.0f;
static float base_b = 1.0f;

inline const char *cluster_id_to_string(uint32_t cluster_id)
{
    switch (cluster_id)
    {
    case OnOff::Id:
        return "OnOff";
    case LevelControl::Id:
        return "LevelControl";
    case ColorControl::Id:
        return "ColorControl";
    default:
        return "UnknownCluster";
    }
}

inline const char *attribute_id_to_string(uint32_t cluster_id, uint32_t attribute_id)
{
    switch (cluster_id)
    {
    case OnOff::Id:
        switch (attribute_id)
        {
        case OnOff::Attributes::OnOff::Id:
            return "OnOff";
        default:
            return "UnknownAttribute";
        }
    case LevelControl::Id:
        switch (attribute_id)
        {
        case LevelControl::Attributes::CurrentLevel::Id:
            return "CurrentLevel";
        default:
            return "UnknownAttribute";
        }
    case ColorControl::Id:
        switch (attribute_id)
        {
        case ColorControl::Attributes::CurrentHue::Id:
            return "CurrentHue";
        case ColorControl::Attributes::CurrentSaturation::Id:
            return "CurrentSaturation";
        case ColorControl::Attributes::RemainingTime::Id:
            return "RemainingTime";
        case ColorControl::Attributes::ColorTemperatureMireds::Id:
            return "ColorTemperatureMireds";
        case ColorControl::Attributes::CurrentX::Id:
            return "CurrentX";
        case ColorControl::Attributes::CurrentY::Id:
            return "CurrentY";
        case ColorControl::Attributes::ColorMode::Id:
            return "ColorMode";
        case ColorControl::Attributes::EnhancedColorMode::Id:
            return "EnhancedColorMode";
        case ColorControl::Attributes::ColorCapabilities::Id:
            return "ColorCapabilities";
        case ColorControl::Attributes::FeatureMap::Id:
            return "FeatureMap";
        default:
            return "UnknownAttribute";
        }
    default:
        return "UnknownCluster";
    }
}

static void wifi_event_log_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        auto *conn = static_cast<wifi_event_sta_connected_t *>(event_data);
        char ssid[33] = {0};
        size_t len = conn->ssid_len < sizeof(ssid) ? conn->ssid_len : sizeof(ssid) - 1;
        memcpy(ssid, conn->ssid, len);
        ssid[len] = '\0';
        ESP_LOGI(TAG, "WiFi connected ssid=%s channel=%d", ssid, conn->channel);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // Force legacy-friendly protocols to avoid HE/ax negotiation quirks
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        ESP_LOGI(TAG, "Configured STA protocol b/g/n");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        auto *disc = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        char ssid[33] = {0};
        if (disc->ssid_len > 0 && disc->ssid_len < sizeof(ssid))
        {
            memcpy(ssid, disc->ssid, disc->ssid_len);
            ssid[disc->ssid_len] = '\0';
            ESP_LOGW(TAG, "WiFi disconnect ssid=%s reason=%d", ssid, disc->reason);
        }
        else
        {
            ESP_LOGW(TAG, "WiFi disconnect reason=%d", disc->reason);
        }
    }
}

static float clamp01(float v)
{
    if (v < 0.0f)
    {
        return 0.0f;
    }
    if (v > 1.0f)
    {
        return 1.0f;
    }
    return v;
}

static void drive_led_strip(uint32_t r, uint32_t g, uint32_t b)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, r);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, g);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_2, b);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_2);
}

static void apply_led_from_state()
{
    float brightness = static_cast<float>(current_brightness) / static_cast<float>(MAX_BRIGHTNESS);
    if (!current_power)
    {
        brightness = 0.0f;
    }

    uint32_t r = static_cast<uint32_t>(clamp01(base_r) * brightness * LEDC_DUTY_MAX);
    uint32_t g = static_cast<uint32_t>(clamp01(base_g) * brightness * LEDC_DUTY_MAX);
    uint32_t b = static_cast<uint32_t>(clamp01(base_b) * brightness * LEDC_DUTY_MAX);

    drive_led_strip(r, g, b);
}

static void hsv_to_rgb(float h_deg, float s, float v, float &r, float &g, float &b)
{
    // h: 0-360, s:0-1, v:0-1
    float c = v * s;
    float h_prime = fmodf(h_deg / 60.0f, 6.0f);
    float x = c * (1.0f - fabsf(fmodf(h_prime, 2.0f) - 1.0f));

    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (0.0f <= h_prime && h_prime < 1.0f)
    {
        r1 = c;
        g1 = x;
        b1 = 0.0f;
    }
    else if (1.0f <= h_prime && h_prime < 2.0f)
    {
        r1 = x;
        g1 = c;
        b1 = 0.0f;
    }
    else if (2.0f <= h_prime && h_prime < 3.0f)
    {
        r1 = 0.0f;
        g1 = c;
        b1 = x;
    }
    else if (3.0f <= h_prime && h_prime < 4.0f)
    {
        r1 = 0.0f;
        g1 = x;
        b1 = c;
    }
    else if (4.0f <= h_prime && h_prime < 5.0f)
    {
        r1 = x;
        g1 = 0.0f;
        b1 = c;
    }
    else
    {
        r1 = c;
        g1 = 0.0f;
        b1 = x;
    }

    float m = v - c;
    r = clamp01(r1 + m);
    g = clamp01(g1 + m);
    b = clamp01(b1 + m);
}

static void xy_to_rgb(float x, float y, float &r, float &g, float &b)
{
    // Convert CIE 1931 xy with Y=1 to sRGB, then normalize into 0..1
    if (y <= 0.0f)
    {
        r = g = b = 0.0f;
        return;
    }

    float Y = 1.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * (1.0f - x - y);

    // sRGB D65 conversion
    float rl = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float gl = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float bl = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;

    rl = fmaxf(0.0f, rl);
    gl = fmaxf(0.0f, gl);
    bl = fmaxf(0.0f, bl);

    float max_val = fmaxf(rl, fmaxf(gl, bl));
    if (max_val > 0.0f)
    {
        rl /= max_val;
        gl /= max_val;
        bl /= max_val;
    }

    r = clamp01(rl);
    g = clamp01(gl);
    b = clamp01(bl);
}

static void temperature_to_rgb(uint16_t mireds, float &r, float &g, float &b)
{
    // Convert mireds to Kelvin, then approximate RGB
    if (mireds == 0)
    {
        r = g = b = 1.0f;
        return;
    }

    float kelvin = 1000000.0f / static_cast<float>(mireds);
    kelvin = fmaxf(1000.0f, fminf(40000.0f, kelvin));
    kelvin /= 100.0f;

    // Approximation from Tanner Helland's algorithm
    if (kelvin <= 66.0f)
    {
        r = 1.0f;
        g = clamp01((99.4708025861f * logf(kelvin) - 161.1195681661f) / 255.0f);
        if (kelvin <= 19.0f)
        {
            b = 0.0f;
        }
        else
        {
            b = clamp01((138.5177312231f * logf(kelvin - 10.0f) - 305.0447927307f) / 255.0f);
        }
    }
    else
    {
        r = clamp01((329.698727446f * powf(kelvin - 60.0f, -0.1332047592f)) / 255.0f);
        g = clamp01((288.1221695283f * powf(kelvin - 60.0f, -0.0755148492f)) / 255.0f);
        b = 1.0f;
    }
}

static void configure_led_pwm(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channels[] = {
        {
            .gpio_num = LED_GPIO_R,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        },
        {
            .gpio_num = LED_GPIO_G,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        },
        {
            .gpio_num = LED_GPIO_B,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CHANNEL_2,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        },
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i)
    {
        ledc_channel_config(&channels[i]);
    }
}

static void button_factory_reset_pressed_cb(void *arg, void *data)
{
    if (!perform_factory_reset)
    {
        ESP_LOGI(TAG, "Factory reset triggered. Release the button to start factory reset.");
        perform_factory_reset = true;
    }
}

static void button_factory_reset_released_cb(void *arg, void *data)
{
    if (perform_factory_reset)
    {
        ESP_LOGI(TAG, "Starting factory reset");
        esp_matter::factory_reset();
        perform_factory_reset = false;
    }
}

static void button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = g_light_endpoint_id;
    uint32_t cluster_id = OnOff::Id;
    uint32_t attribute_id = OnOff::Attributes::OnOff::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(attribute, &val);
    val.val.b = !val.val.b;
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

static esp_err_t register_reset_button()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    /* Register button device */
    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create button device");
        return ESP_FAIL;
    }

    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, button_toggle_cb, NULL);

    esp_err_t err = ESP_OK;
    err |= iot_button_register_cb(handle, BUTTON_LONG_PRESS_HOLD, NULL, button_factory_reset_pressed_cb, NULL);
    err |= iot_button_register_cb(handle, BUTTON_PRESS_UP, NULL, button_factory_reset_released_cb, NULL);

    return err;
}

static esp_err_t light_set_power(bool power_on_off)
{
    current_power = power_on_off;

    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t light_set_brightness(uint8_t brightness)
{
    if (brightness > MAX_BRIGHTNESS)
    {
        brightness = MAX_BRIGHTNESS;
    }
    current_brightness = brightness;

    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t light_set_hue(uint8_t hue)
{
    if (hue > MAX_HUE)
    {
        hue = MAX_HUE;
    }
    current_hue = hue;

    float hue_deg = (static_cast<float>(hue) / static_cast<float>(MAX_HUE)) * 360.0f;
    float sat = static_cast<float>(current_saturation) / static_cast<float>(MAX_SATURATION);
    hsv_to_rgb(hue_deg, sat, 1.0f, base_r, base_g, base_b);
    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t light_set_saturation(uint8_t saturation)
{
    if (saturation > MAX_SATURATION)
    {
        saturation = MAX_SATURATION;
    }
    current_saturation = saturation;

    float hue_deg = (static_cast<float>(current_hue) / static_cast<float>(MAX_HUE)) * 360.0f;
    float sat = static_cast<float>(saturation) / static_cast<float>(MAX_SATURATION);
    hsv_to_rgb(hue_deg, sat, 1.0f, base_r, base_g, base_b);
    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t light_set_temperature(uint16_t temp)
{
    current_temperature = temp;

    temperature_to_rgb(temp, base_r, base_g, base_b);

    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t light_set_xy(uint16_t x, uint16_t y)
{
    current_x = x;
    current_y = y;

    float xf = static_cast<float>(x) / 65535.0f;
    float yf = static_cast<float>(y) / 65535.0f;

    xy_to_rgb(xf, yf, base_r, base_g, base_b);

    apply_led_from_state();

    return ESP_OK;
}

static esp_err_t attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                  esp_matter_attr_val_t *value)
{
    // Only handle updates for our light endpoint
    if (endpoint_id != g_light_endpoint_id)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Attribute update received: cluster=%s(0x%08X), attribute=%s(0x%08X)",
             cluster_id_to_string(cluster_id), cluster_id, attribute_id_to_string(cluster_id, attribute_id),
             attribute_id);

    if (cluster_id == OnOff::Id)
    {
        if (attribute_id == OnOff::Attributes::OnOff::Id)
        {
            return light_set_power(value->val.b);
        }
    }
    else if (cluster_id == LevelControl::Id)
    {
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id)
        {
            return light_set_brightness(value->val.u8);
        }
    }
    else if (cluster_id == ColorControl::Id)
    {
        if (attribute_id == ColorControl::Attributes::CurrentX::Id)
        {
            return light_set_xy(value->val.u16, current_y);
        }
        else if (attribute_id == ColorControl::Attributes::CurrentY::Id)
        {
            return light_set_xy(current_x, value->val.u16);
        }
        else if (attribute_id == ColorControl::Attributes::RemainingTime::Id)
        {
            // Ignore RemainingTime updates
            return ESP_OK;
        }
        else if (attribute_id == ColorControl::Attributes::CurrentHue::Id)
        {
            return light_set_hue(value->val.u8);
        }
        else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id)
        {
            return light_set_saturation(value->val.u8);
        }
        else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id)
        {
            return light_set_temperature(value->val.u16);
        }
    }

    ESP_LOGW(TAG, "Attribute update not handled");

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t set_light_defaults()
{
    esp_err_t err = ESP_OK;

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);

    /* Setting brightness */
    attribute_t *attribute =
        attribute::get(g_light_endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    attribute::get_val(attribute, &val);

    err |= light_set_brightness(val.val.u8);

    /* Setting color mode */
    attribute = attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    attribute::get_val(attribute, &val);
    uint8_t color_mode = val.val.u8;

    if (color_mode == (uint8_t)ColorControl::ColorMode::kColorTemperature)
    {
        // Force color mode if old state persisted color temperature
        color_mode = (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY;
        esp_matter_attr_val_t new_mode_val = esp_matter_uint8(color_mode);
        attribute::update(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id,
                          &new_mode_val);
        attribute::update(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::EnhancedColorMode::Id,
                          &new_mode_val);
    }

    if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY)
    {
        /* Setting XY coordinates */
        attribute = attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
        attribute::get_val(attribute, &val);
        auto x = val.val.u16;

        attribute = attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
        attribute::get_val(attribute, &val);
        auto y = val.val.u16;

        err |= light_set_xy(x, y);
    }
    else if (color_mode == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation)
    {
        /* Setting hue */
        attribute = attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
        attribute::get_val(attribute, &val);

        err |= light_set_hue(val.val.u8);

        /* Setting saturation */
        attribute =
            attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
        attribute::get_val(attribute, &val);

        err |= light_set_saturation(val.val.u8);
    }
    else if (color_mode == (uint8_t)ColorControl::ColorMode::kColorTemperature)
    {
        /* Setting color temperature */
        attribute =
            attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
        attribute::get_val(attribute, &val);

        err |= light_set_temperature(val.val.u16);
    }
    else
    {
        ESP_LOGE(TAG, "Color mode not supported");
    }

    /* Setting power */
    attribute = attribute::get(g_light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::get_val(attribute, &val);

    err |= light_set_power(val.val.b);

    return err;
}

static void event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type)
    {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        {
            /* Print onboarding codes so the user can commission the device */
            chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);
            PrintOnboardingCodes(flags);
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
    {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
        {
            chip::CommissioningWindowManager &commissionMgr =
                chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen())
            {
                /*
                After removing last fabric, this example does not remove the Wi-Fi credentials
                and still has IP connectivity so, only advertising on DNS-SD.
                */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(
                    kTimeoutSeconds, chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR)
                {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

static esp_err_t identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                   uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *value, void *priv_data)
{
    if (type != PRE_UPDATE)
    {
        return ESP_OK;
    }

    return attribute_update(endpoint_id, cluster_id, attribute_id, value);
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the NVS layer */
    nvs_flash_init();

    /* Handle Wi-Fi events */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_log_handler, nullptr));

    /* Configure LED PWM */
    configure_led_pwm();

    /* Initialize reset button */
    register_reset_button();

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;

    // Node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    ABORT_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    extended_color_light::config_t light_config;

    light_config.on_off.on_off = DEFAULT_POWER;
    light_config.on_off_lighting.start_up_on_off = nullptr;

    light_config.level_control.current_level = DEFAULT_BRIGHTNESS;
    light_config.level_control.on_level = DEFAULT_BRIGHTNESS;

    light_config.level_control_lighting.start_up_current_level = DEFAULT_BRIGHTNESS;

    light_config.color_control.color_mode = (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY;
    light_config.color_control.enhanced_color_mode = (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY;

    light_config.color_control_xy.current_x = DEFAULT_X;
    light_config.color_control_xy.current_y = DEFAULT_Y;

    light_config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr;

    // Endpoint handles can be used to add/modify clusters.
    endpoint_t *endpoint = extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create extended color light endpoint"));

    g_light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "LED strip light created with endpoint_id %d", g_light_endpoint_id);

    /* Mark deferred persistence for some attributes that might be changed rapidly */

    attribute_t *current_level_attribute =
        attribute::get(g_light_endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    attribute::set_deferred_persistence(current_level_attribute);

    attribute_t *current_x_attribute =
        attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
    attribute::set_deferred_persistence(current_x_attribute);

    attribute_t *current_y_attribute =
        attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
    attribute::set_deferred_persistence(current_y_attribute);

    attribute_t *hue_attribute =
        attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
    attribute::set_deferred_persistence(hue_attribute);

    attribute_t *saturation_attribute =
        attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
    attribute::set_deferred_persistence(saturation_attribute);

    attribute_t *color_temp_attribute =
        attribute::get(g_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
    attribute::set_deferred_persistence(color_temp_attribute);

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto *dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif

    /* Matter start */
    err = esp_matter::start(event_cb);
    ABORT_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /* Starting with default values */
    err = set_light_defaults();
    ABORT_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to set light defaults, err:%d", err));

    while (true)
    {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
