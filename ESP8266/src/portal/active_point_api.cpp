#include "active_point_api.h"
#include "ESPAsyncTCP.h"
#include "WebHandlerImpl.h"
#include <IPAddress.h>
#include <LittleFS.h>
#include "AsyncJson.h"

#include "setup.h"
#include "Logging.h"
#include "master_i2c.h"
#include "utils.h"
#include "config.h"
#include "wifi_helpers.h"
#include "resources.h"

extern bool exit_portal_flag;
extern bool start_connect_flag;
extern wl_status_t wifi_connect_status;
extern bool factory_reset_flag;

SlaveData runtime_data;
extern SlaveData data;
extern MasterI2C masterI2C;
extern Settings sett;
extern CalculatedData cdata;

#define IMPULS_LIMIT_1 3 // Если пришло импульсов меньше 3, то перед нами 10л/имп. Если больше, то 1л/имп.

uint8_t get_auto_factor(const uint32_t runtime_impulses, const uint32_t impulses)
{
    return (runtime_impulses - impulses <= IMPULS_LIMIT_1) ? 10 : 1;
}

/**
 * @brief Запрос состояния подключения к роутеру.
 *        После успеха или не успеха - переадресация на другую страницу.
 *
 * @param request запрос
 */
void onGetApiConnectStatus(AsyncWebServerRequest *request)
{
    LOG_INFO(F("GET ") << request->url());

    DynamicJsonDocument json_doc(JSON_SMALL_STATIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();

    if (start_connect_flag)
    {
        ret["status"] = F("выполняется подключение...");
        LOG_INFO(F("WIFI: connecting..."));
    }
    else
    {
        LOG_INFO(F("WIFI: wifi_connect_status=") << wifi_connect_status);

        if (wifi_connect_status == WL_CONNECTED)
        {
            ret[F("redirect")] = F("/setup_blue_type.html");
        }
        else
        {
            ret[F("redirect")] = F("/wifi_settings.html");
        }
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
};

/**
 * @brief Список Wi-Fi сетей
 *
 * @param request запрос
 */
void onGetApiNetworks(AsyncWebServerRequest *request)
{
    LOG_INFO(F("GET ") << request->url());

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED)
    {
        WiFi.scanNetworks(true);
        request->send(200, "", F("[]"));
    }
    else if (n)
    {
        DynamicJsonDocument json_doc(JSON_DYNAMIC_MSG_BUFFER);
        JsonArray array = json_doc.to<JsonArray>();

        for (int i = 0; i < n; ++i)
        {
            LOG_INFO(WiFi.SSID(i) << " " << WiFi.RSSI(i));
            JsonObject obj = array.createNestedObject();
            obj["ssid"] = WiFi.SSID(i);
            obj["level"] = int(round(map(WiFi.RSSI(i), -100, -50, 1, 4)));
            obj["wifi_channel"] = WiFi.channel();
        }

        write_ssid_to_file();

        WiFi.scanDelete();

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(json_doc, *response);
        request->send(response);
    }
};

/**
 * @brief Подключение к точки доступа
 *
 * @param request запрос
 */
void onPostApiSetupConnect(AsyncWebServerRequest *request)
{
    LOG_INFO(F("POST ") << request->url());

    DynamicJsonDocument json_doc(JSON_SMALL_STATIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();
    JsonObject errorsObj = ret.createNestedObject(F("errors"));

    // Если канал WiFi отличен от текущего канала AP ESP, то возможно отключение телефона
    uint8_t channel = sett.wifi_channel;

    applySettings(request, errorsObj);
    
    bool wizard = find_wizard_param(request);
    
    if (!errorsObj.size())
    {
        ret.remove(F("errors"));

        String params;

        if (channel != sett.wifi_channel)
        {
            if (wizard) 
            {
                params += F("wizard=true&error=Канал Wi-Fi роутера отличается от текущего соединения. Если телефон потеряет связь с Ватериусом, подключитесь заново.");
            }
            else 
            {
                params += F("error=Канал Wi-Fi роутера отличается от текущего соединения. Если телефон потеряет связь с Ватериусом, подключитесь заново.");
            }
        }
        else 
        {
            if (wizard) 
            {
                params += F("wizard=true");
            }
        }

        if (params.length())
        {
            ret[F("redirect")] = F("/api/call_connect?") + params;
        }
        else 
        {
            ret[F("redirect")] = F("/api/call_connect");
        }
    }
    
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
}

/**
 * @brief Подключение к точки доступа
 *
 * @param request запрос
 */
void onGetApiCallConnect(AsyncWebServerRequest *request)
{
    start_connect_flag = true;
    wifi_connect_status = WL_DISCONNECTED;
    LOG_INFO(F("Start connect"));

    bool wizard = find_wizard_param(request);
    if (wizard)
    {
        request->redirect("/wifi_connect.html?wizard=true");
    }
    else
    {
        request->redirect("/wifi_connect.html");
    }
}

/**
 * @brief Список диагностических сообщений на Главной странице вебсервера
 *        
 * @param request запрос
 */
void onGetApiMainStatus(AsyncWebServerRequest *request)
{
    LOG_INFO(F("GET ") << request->url());

    DynamicJsonDocument json_doc(JSON_SMALL_STATIC_MSG_BUFFER);
    JsonArray array = json_doc.to<JsonArray>();

    wl_status_t status = WiFi.status();
    LOG_INFO(F("WIFI: status=") << status);
    
    if (status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST || status == WL_WRONG_PASSWORD)
    {
        JsonObject obj = array.createNestedObject();
        obj["error"] = F("Ошибка подключения к Wi-Fi");
        obj["link_text"] = F("Настроить");
        obj["link"] = F("/wifi_settings.html?status_code=") + String(status);
    }
    else
    {
        if (sett.factor1 == AUTO_IMPULSE_FACTOR)
        {
            if (status == WL_CONNECTED)
            {
                JsonObject obj = array.createNestedObject();
                obj["error"] = F("Ватериус успешно подключился к Wi-Fi. Теперь настроим счётчики.");
                obj["link_text"] = F("Настроить");
                obj["link"] = F("/setup_blue_type.html");
            }
            else 
            {
                JsonObject obj = array.createNestedObject();
                obj["error"] = F("Ватериус ещё не настроен");
                obj["link_text"] = F("Приступить");
                obj["link"] = F("/captive_portal_start.html");
            }
        }
    }

    LOG_INFO(F("JSON: Mem usage: ") << json_doc.memoryUsage());
    LOG_INFO(F("JSON: Size: ") << measureJson(json_doc));

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
}

void onGetApiStatus0(AsyncWebServerRequest *request)
{
    onGetApiStatus(request, 0);
}

void onGetApiStatus1(AsyncWebServerRequest *request)
{
    onGetApiStatus(request, 1);
}

/**
 * @brief Запрос состояния входа
 *
 * @param request запрос
 */
void onGetApiStatus(AsyncWebServerRequest *request, const int index)
{
    LOG_INFO(F("GET ") << request->url());

    DynamicJsonDocument json_doc(JSON_SMALL_STATIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();

    uint16_t factor;
    if (masterI2C.getSlaveData(runtime_data))
    {
        if (index == 0)
        {
            if (sett.factor0 == AS_COLD_CHANNEL)
            {
                if (sett.factor1 == AUTO_IMPULSE_FACTOR)
                {
                    factor = get_auto_factor(runtime_data.impulses0, data.impulses0);
                }
                else
                {
                    factor = sett.factor1;
                }
            }
            else
            {
                factor = sett.factor0;
            }
            ret[F("state")] = int(runtime_data.impulses0 > data.impulses0);
            ret[F("factor")] = factor;
            ret[F("impulses")] = runtime_data.impulses0 - data.impulses0;
        }
        else if (index == 1)
        {
            if (sett.factor1 == AUTO_IMPULSE_FACTOR)
            {
                factor = get_auto_factor(runtime_data.impulses1, data.impulses1);
            }
            else // повторная настройка
            {
                factor = sett.factor1;
            }
            ret[F("state")] = int(runtime_data.impulses1 > data.impulses1);
            ret[F("factor")] = factor;
            ret[F("impulses")] = runtime_data.impulses1 - data.impulses1;
        }
        // root[F("elapsed")] = (uint32_t)(SETUP_TIME_SEC - millis() / 1000.0);
    }
    else
    {
        ret[F("error")] = F("Ошибка связи с МК");
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
};

/**
 * @brief Запрос сохранения настроек
 *
 * @param request POST, данные в x-www-form-urlencoded
 *
 *      Удаляем поля где значение null
 *      Проверяем настройки на корректность
 *      :param form_data: dict
 *      :return:
 *      {...form_data...} - успех
 *
 *      Если есть ошибки:
 *      {...form_data...
 *          "errors": {
 *              "serial1": "ошибка"
 *          }
 *      }
 */

void save_param(AsyncWebParameter *p, char *dest, size_t size, JsonObject &errorsObj, bool required /*true*/)
{
    if (p->value().length() >= size)
    {
        LOG_ERROR(FPSTR(ERROR_LENGTH_63) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_LENGTH_63);
    }
    else if (required && p->value().length() == 0)
    {
        LOG_ERROR(FPSTR(ERROR_EMPTY) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_EMPTY);
    }
    else
    {   
        String value(p->value());
        value.trim();  //чтобы пользователи случайно не ввели пробел
        strncpy0(dest, value.c_str(), size);
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << value);
    }
}

void save_param(AsyncWebParameter *p, uint16_t &v, JsonObject &errorsObj)
{
    if (p->value().toInt() == 0)
    {
        LOG_ERROR(FPSTR(ERROR_VALUE) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_VALUE);
    }
    else
    {
        v = p->value().toInt();
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << v);
    }
}

void save_param(AsyncWebParameter *p, uint8_t &v, JsonObject &errorsObj, const bool zero_ok)
{
    if (!zero_ok && p->value().toInt() == 0)
    {
        LOG_ERROR(FPSTR(ERROR_VALUE) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_VALUE);
    }
    else
    {
        v = p->value().toInt();
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << v);
    }
}

void save_bool_param(AsyncWebParameter *p, uint8_t &v, JsonObject &errorsObj)
{
    if (p->value().toInt() > 1)
    {
        LOG_ERROR(FPSTR(ERROR_VALUE) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_VALUE);
    }
    else
    {
        v = p->value().toInt();
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << v);
    }
}

void save_param(AsyncWebParameter *p, float &v, JsonObject &errorsObj)
{
    /* Позволяем вводить 0.0 у счётчиков.
    if (p->value().toFloat() == 0.0)
    {
        LOG_ERROR(FPSTR(ERROR_VALUE) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_VALUE);
    }
    else */
    {
        v = p->value().toFloat();
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << v);
    }
}

void save_ip_param(AsyncWebParameter *p, uint32_t &v, JsonObject &errorsObj)
{
    IPAddress ip;
    if (ip.fromString(p->value()))
    {
        v = ip.v4();
        LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << ip.toString());
    }
    else
    {
        LOG_ERROR(FPSTR(ERROR_VALUE) << ": " << p->name());
        errorsObj[p->name()] = FPSTR(ERROR_VALUE);
    }
}

bool find_wizard_param(AsyncWebServerRequest *request)
{
    for (size_t i = 0; i < request->params(); i++)
    {
        AsyncWebParameter *p = request->getParam(i);
        if (p->name() == FPSTR(PARAM_WIZARD))
        {
            return p->value() == FPSTR(PARAM_TRUE);
        }
    }
    return false;
}

void applySettings(AsyncWebServerRequest *request, JsonObject &errorsObj)
{
    const int params = request->params();

    LOG_INFO(F("Apply ") << params << " parameters");

    // Вначале bool, чтобы дальше проверять только требуемые параметры
    for (int i = 0; i < params; i++)
    {
        AsyncWebParameter *p = request->getParam(i);
        const String &name = p->name();

        if (name == FPSTR(PARAM_WATERIUS_ON))
        {
            save_bool_param(p, sett.waterius_on, errorsObj);
        }
        else if (name == FPSTR(PARAM_HTTP_ON))
        {
            save_bool_param(p, sett.http_on, errorsObj);
        }
        else if (name == FPSTR(PARAM_MQTT_ON))
        {
            save_bool_param(p, sett.mqtt_on, errorsObj);
        }
        else if (name == FPSTR(PARAM_BLYNK_ON))
        {
            save_bool_param(p, sett.blynk_on, errorsObj);
        }
        else if (name == FPSTR(PARAM_DHCP_OFF))
        {
            save_bool_param(p, sett.dhcp_off, errorsObj);
        }
        else if (name == FPSTR(PARAM_MQTT_AUTO_DISCOVERY))
        {
            save_bool_param(p, sett.mqtt_auto_discovery, errorsObj);
        }
    }

    for (int i = 0; i < params; i++)
    {
        AsyncWebParameter *p = request->getParam(i);
        const String &name = p->name();

        if (sett.waterius_on)
        {
            if (name == FPSTR(PARAM_WATERIUS_HOST))
            {
                save_param(p, sett.waterius_host, HOST_LEN, errorsObj);
            }
            else if (name == FPSTR(PARAM_WATERIUS_EMAIL))
            {
                save_param(p, sett.waterius_email, EMAIL_LEN, errorsObj);
            }
        }

        if (sett.blynk_on)
        {
            if (name == FPSTR(PARAM_BLYNK_KEY))
            {
                save_param(p, sett.blynk_key, BLYNK_KEY_LEN, errorsObj, false);
            }
            else if (name == FPSTR(PARAM_BLYNK_HOST))
            {
                save_param(p, sett.blynk_host, HOST_LEN, errorsObj);
            }
        }

        if (sett.http_on)
        {
            if (name == FPSTR(PARAM_HTTP_URL))
            {
                save_param(p, sett.http_url, HOST_LEN, errorsObj);
            }
        }

        if (sett.mqtt_on)
        {
            if (name == FPSTR(PARAM_MQTT_HOST))
            {
                save_param(p, sett.mqtt_host, HOST_LEN, errorsObj);
            }
            else if (name == FPSTR(PARAM_MQTT_PORT))
            {
                save_param(p, sett.mqtt_port, errorsObj);
            }
            else if (name == FPSTR(PARAM_MQTT_LOGIN))
            {
                save_param(p, sett.mqtt_login, MQTT_LOGIN_LEN, errorsObj, false);
            }
            else if (name == FPSTR(PARAM_MQTT_PASSWORD))
            {
                save_param(p, sett.mqtt_password, MQTT_PASSWORD_LEN, errorsObj, false);
            }
            else if (name == FPSTR(PARAM_MQTT_TOPIC))
            {
                save_param(p, sett.mqtt_topic, MQTT_TOPIC_LEN, errorsObj, false);
            }

            if (sett.mqtt_auto_discovery)
            {
                if (name == FPSTR(PARAM_MQTT_DISCOVERY_TOPIC))
                {
                    save_param(p, sett.mqtt_discovery_topic, MQTT_TOPIC_LEN, errorsObj, false);
                }
            }
        }

        if (sett.dhcp_off)
        {
            if (name == FPSTR(PARAM_IP))
            {
                save_ip_param(p, sett.ip, errorsObj);
            }
            else if (name == FPSTR(PARAM_GATEWAY))
            {
                save_ip_param(p, sett.gateway, errorsObj);
            }
            else if (name == FPSTR(PARAM_MASK))
            {
                save_ip_param(p, sett.mask, errorsObj);
            }
        }

        if (name == FPSTR(PARAM_CHANNEL0_START))
        {
            save_param(p, sett.channel0_start, errorsObj);
            sett.impulses0_start = runtime_data.impulses0;
            sett.impulses0_previous = sett.impulses0_start;
            LOG_INFO("impulses0_start=" << sett.impulses0_start);
        }
        else if (name == FPSTR(PARAM_CHANNEL1_START))
        {
            save_param(p, sett.channel1_start, errorsObj);
            sett.impulses1_start = runtime_data.impulses1;
            sett.impulses1_previous = sett.impulses1_start;
            LOG_INFO("impulses1_start=" << sett.impulses1_start);
        }

        else if (name == FPSTR(PARAM_SERIAL0))
        {
            save_param(p, sett.serial0, SERIAL_LEN, errorsObj, false);
        }
        else if (name == FPSTR(PARAM_SERIAL1))
        {
            save_param(p, sett.serial1, SERIAL_LEN, errorsObj, false);
        }
        else if (name == FPSTR(PARAM_WAKEUP_PER_MIN))
        {
            save_param(p, sett.wakeup_per_min, errorsObj);
            sett.set_wakeup = sett.wakeup_per_min;
        }

        else if (name == FPSTR(PARAM_NTP_SERVER))
        {
            save_param(p, sett.ntp_server, HOST_LEN, errorsObj);
        }

        else if (name == FPSTR(PARAM_SSID))
        {
            save_param(p, sett.wifi_ssid, WIFI_SSID_LEN, errorsObj);
        }
        else if (name == FPSTR(PARAM_PASSWORD))
        {
            save_param(p, sett.wifi_password, WIFI_PWD_LEN, errorsObj, false);
        }

        else if (name == FPSTR(PARAM_WIFI_PHY_MODE))
        {
            save_param(p, sett.wifi_phy_mode, errorsObj, true);
        }

        else if (name == FPSTR(PARAM_COUNTER0_NAME))
        {
            save_param(p, sett.counter0_name, errorsObj, true);
        }
        else if (name == FPSTR(PARAM_COUNTER1_NAME))
        {
            save_param(p, sett.counter1_name, errorsObj, true);
        }

        else if (name == FPSTR(PARAM_COUNTER0_TYPE))
        {
            if (!masterI2C.setCountersType(p->value().toInt(), data.counter_type1))
            {
                LOG_ERROR(FPSTR(ERROR_ATTINY_ERROR) << ": " << p->name());
                errorsObj[p->name()] = FPSTR(ERROR_ATTINY_ERROR);
            }
            else
            {
                data.counter_type0 = p->value().toInt();
                LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << p->value());
            }
        }
        else if (name == FPSTR(PARAM_COUNTER1_TYPE))
        {
            if (!masterI2C.setCountersType(data.counter_type0, p->value().toInt()))
            {
                LOG_ERROR(FPSTR(ERROR_ATTINY_ERROR) << ": " << p->name());
                errorsObj[p->name()] = FPSTR(ERROR_ATTINY_ERROR);
            }
            else
            {
                data.counter_type1 = p->value().toInt();
                LOG_INFO(FPSTR(PARAM_SAVED) << p->name() << F("=") << p->value());
            }
        }
        else if (name == FPSTR(PARAM_FACTOR0))
        {
            save_param(p, sett.factor0, errorsObj);
        }
        else if (name == FPSTR(PARAM_FACTOR1))
        {
            save_param(p, sett.factor1, errorsObj);
        }
    }

    store_config(sett);
}

void onPostApiSetup(AsyncWebServerRequest *request)
{
    LOG_INFO(F("POST ") << request->url());
    DynamicJsonDocument json_doc(JSON_DYNAMIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();
    JsonObject errorsObj = ret.createNestedObject("errors");

    applySettings(request, errorsObj);

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
}

void onPostApiSetCounterName0(AsyncWebServerRequest *request)
{
    onPostApiSetCounterName(request, 0);
}

void onPostApiSetCounterName1(AsyncWebServerRequest *request)
{
    onPostApiSetCounterName(request, 1);
}

void onPostApiSetCounterName(AsyncWebServerRequest *request, const uint8_t index)
{
    LOG_INFO(F("POST ") << request->url());
    DynamicJsonDocument json_doc(JSON_DYNAMIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();
    JsonObject errorsObj = ret.createNestedObject("errors");

    applySettings(request, errorsObj);

    if (index == 0)
    {   
        switch (sett.counter0_name)
        {
            case CounterName::WATER_COLD:
            case CounterName::WATER_HOT:
            case CounterName::PORTABLE_WATER:
                ret[F("redirect")] = F("/setup_red_water.html");
                break;
            default:
                ret[F("redirect")] = F("/setup_red.html");
        }
    } 
    else 
    {
        switch (sett.counter1_name)
        {
            case CounterName::WATER_COLD:
            case CounterName::WATER_HOT:
            case CounterName::PORTABLE_WATER:
                ret[F("redirect")] = F("/setup_blue_water.html");
                break;
            default:
                ret[F("redirect")] = F("/setup_blue.html");
        }
    }

    bool wizard = find_wizard_param(request);
    if (wizard)
    {
        ret[F("redirect")] = ret[F("redirect")] + F("?wizard=true");
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);
    request->send(response);
}

void onGetApiTurnOff(AsyncWebServerRequest *request)
{
    LOG_INFO(F("GET ") << request->url());
    exit_portal_flag = true;
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "");
    request->send(response);
}

void onPostApiReset(AsyncWebServerRequest *request)
{
    LOG_INFO(F("POST ") << request->url());

    DynamicJsonDocument json_doc(JSON_SMALL_STATIC_MSG_BUFFER);
    JsonObject ret = json_doc.to<JsonObject>();

    ret[F("redirect")] = F("/");

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(json_doc, *response);

    factory_reset_flag = true;

    request->send(response);
}