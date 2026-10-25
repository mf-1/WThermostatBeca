#ifndef __WCLOCK_H__
#define __WCLOCK_H__

#include "Arduino.h"
#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <coredecls.h>
#elif ESP32
#include <WiFi.h>
#endif
#include <time.h>
#include "TimeLib.h"
#include "WDevice.h"
#include "WNetwork.h"

const char* DEFAULT_NTP_SERVER = "pool.ntp.org";
const char* DEFAULT_TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
const byte STD_MONTH = 0;
const byte STD_WEEK = 1;
const byte STD_WEEKDAY = 2;
const byte STD_HOUR = 3;
const byte DST_MONTH = 4;
const byte DST_WEEK = 5;
const byte DST_WEEKDAY = 6;
const byte DST_HOUR = 7;
const byte *DEFAULT_DST_RULE = (const byte[]){10, 0, 0, 3, 3, 0, 0, 2};
const byte *DEFAULT_NIGHT_SWITCHES = (const byte[]){22, 00, 7, 00};
//Before first sync the system clock starts at 1970
const time_t MIN_VALID_EPOCH_TIME = 1700000000;
const byte TIME_ZONE_PRESET_CUSTOM = 0xFF;

struct WTimeZonePreset {
	const char* name;
	const char* posix;
};

const WTimeZonePreset TIME_ZONE_PRESETS[] = {
	{"Central Europe (Rome, Berlin, Paris, Vienna, Madrid)", DEFAULT_TIME_ZONE},
	{"UK, Ireland (London, Dublin)", "GMT0BST,M3.5.0/1,M10.5.0"},
	{"Western Europe (Lisbon)", "WET0WEST,M3.5.0/1,M10.5.0"},
	{"Eastern Europe (Athens, Helsinki, Bucharest, Kyiv)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
	{"UTC+3 (Moscow, Istanbul, Minsk)", "MSK-3"},
	{"US Eastern (New York)", "EST5EDT,M3.2.0,M11.1.0"},
	{"US Central (Chicago)", "CST6CDT,M3.2.0,M11.1.0"},
	{"US Mountain (Denver)", "MST7MDT,M3.2.0,M11.1.0"},
	{"US Pacific (Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
	{"Australia East (Sydney, Melbourne)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
	{"UTC", "UTC0"},
};
const byte TIME_ZONE_PRESET_COUNT = sizeof(TIME_ZONE_PRESETS) / sizeof(TIME_ZONE_PRESETS[0]);

class WClock: public WDevice {
public:
	typedef std::function<void(void)> THandlerFunction;

	WClock(WNetwork* network, bool supportNightMode)
		: WDevice(network, "clock", "clock", DEVICE_TYPE_TEXT_DISPLAY) {
		setMainDevice(false);
		setVisibility(MQTT);
		//Settings are stored by position: keep order and types of all settings
		//of former versions, even if only needed for migration
		this->ntpServer = network->settings()->setString("ntpServer", DEFAULT_NTP_SERVER);
		this->ntpServer->readOnly(true);
		this->ntpServer->visibility(MQTT);
		this->addProperty(ntpServer);
		//former: get time zone via worldtimeapi.org
		this->useTimeZoneServer = network->settings()->setBoolean("useTimeZoneServer", false);
		//former: time zone server URL, now POSIX time zone e.g. 'CET-1CEST,M3.5.0,M10.5.0/3'
		this->timeZone = network->settings()->setString("timeZone", DEFAULT_TIME_ZONE);
		this->timeZone->readOnly(true);
		this->timeZone->visibility(MQTT);
		this->addProperty(timeZone);
		_epochTimeFormatted = WProps::createStringProperty("epochTimeFormatted", "epochTimeFormatted");
		_epochTimeFormatted->readOnly(true);
		_epochTimeFormatted->onValueRequest([this](WProperty* p) {updateFormattedTime();});
		this->addProperty(_epochTimeFormatted);
		this->validTime = WProps::createOnOffProperty("validTime", "validTime");
		this->validTime->asBool(false);
		this->validTime->readOnly(true);
		this->addProperty(validTime);
		//former fixed offset and day saving time rule, only for migration
		this->rawOffset = WProps::createIntegerProperty("raw_offset", "rawOffset");
		this->rawOffset->asInt(3600);
		network->settings()->add(this->rawOffset);
		this->dstOffset = WProps::createIntegerProperty("dst_offset", "dstOffset");
		this->dstOffset->asInt(3600);
		network->settings()->add(this->dstOffset);
		this->useDaySavingTimes = network->settings()->setBoolean("useDaySavingTimes", false);
		this->dstRule = network->settings()->setByteArray("dstRule", 8, DEFAULT_DST_RULE);
		migrateTimeZone();
		//HtmlPages
    WPage* configPage = new WPage(this->id(), "Configure clock");
    configPage->setPrintPage(std::bind(&WClock::printConfigPage, this, std::placeholders::_1, std::placeholders::_2));
    configPage->setSubmittedPage(std::bind(&WClock::saveConfigPage, this, std::placeholders::_1, std::placeholders::_2));
    network->addCustomPage(configPage);

		lastTry = 0;
		timeConfigured = false;
		timeSynced = false;
		lastDst = -1;
		//enableNightMode
		this->enableNightMode = nullptr;
		this->nightMode = nullptr;
		this->nightSwitches = nullptr;
		if (supportNightMode) {
			this->enableNightMode = network->settings()->setBoolean("enableNightMode", true);
			this->nightMode = WProps::createBooleanProperty("nightMode", "nightMode");
			this->addProperty(this->nightMode);
			this->nightSwitches = network->settings()->setByteArray("nightSwitches", 4, DEFAULT_NIGHT_SWITCHES);
		}
		//Called by SNTP after every sync (hourly), handled in loop
		settimeofday_cb([this](bool fromSntp) {
			if (fromSntp) {
				this->timeSynced = true;
			}
		});
	}

	void loop(unsigned long now) {
		if (!timeConfigured) {
			if (WiFi.status() == WL_CONNECTED) {
				configureTime();
			}
			return;
		}
		if ((lastTry == 0) || (now - lastTry > 10000)) {
			bool timeUpdated = false;
			if (timeSynced) {
				timeSynced = false;
				validTime->asBool(time(nullptr) > MIN_VALID_EPOCH_TIME);
				updateFormattedTime();
				network()->debug(F("NTP time synced: %s"), _epochTimeFormatted->c_str());
				timeUpdated = true;
			}
			if (isValidTime()) {
				//Day saving time switched: local time jumped
				int dst = localTime().tm_isdst;
				if (dst != lastDst) {
					lastDst = dst;
					timeUpdated = true;
				}
			}
			//check nightMode
			if ((isValidTime()) && (this->enableNightMode) && (this->enableNightMode->asBool())) {
				this->nightMode->asBool(this->isTimeBetween(this->nightSwitches->byteArrayValue(0), this->nightSwitches->byteArrayValue(1),
																												this->nightSwitches->byteArrayValue(2), this->nightSwitches->byteArrayValue(3)));
			}
			if (timeUpdated) {
				_notifyOnTimeUpdate();
			} else {
				_notifyOnMinuteUpdate();
			}
			lastTry = millis();
		}
	}

	void setOnTimeUpdate(THandlerFunction onTimeUpdate) {
		_onTimeUpdate = onTimeUpdate;
	}

	void setOnMinuteTrigger(THandlerFunction onMinuteTrigger) {
		_onMinuteTrigger = onMinuteTrigger;
	}

	unsigned long epochTime() {
		//local time as epoch seconds, so the static helpers below work on local time
		if (!isValidTime()) {
			return 0;
		}
		struct tm lt = localTime();
		tmElements_t te;
		te.Second = lt.tm_sec;
		te.Minute = lt.tm_min;
		te.Hour = lt.tm_hour;
		te.Day = lt.tm_mday;
		te.Month = lt.tm_mon + 1;
		te.Year = lt.tm_year + 1900 - 1970;
		return makeTime(te);
	}

	byte weekDay() {
		return weekDayOf(epochTime());
	}

	static byte weekDayOf(unsigned long epochTime) {
		//weekday from 0 to 6, 0 is Sunday
		return (((epochTime / 86400L) + 4) % 7);
	}

	const char* weekDayNameShort() {
		return weekDayNameShortOf(weekDay());
	}

	const char* weekDayNameShortOf(byte weekDay) {
		switch (weekDay) {
			case 1: return PSTR("Mo");
			case 2: return PSTR("Di");
			case 3: return PSTR("Mi");
			case 4: return PSTR("Do");
			case 5: return PSTR("Fr");
			case 6: return PSTR("Sa");
			case 0: return PSTR("So");
			default: return PSTR("n.a.");
		}
	}

	byte hours() {
		return hoursOf(epochTime());
	}

	static byte hoursOf(unsigned long epochTime) {
		return ((epochTime % 86400L) / 3600);
	}

	byte minutes() {
		return minutesOf(epochTime());
	}

	static byte minutesOf(unsigned long epochTime) {
		return ((epochTime % 3600) / 60);
	}

	byte seconds() {
		return secondsOf(epochTime());
	}

	static byte secondsOf(unsigned long epochTime) {
		return (epochTime % 60);
	}

	int yearOf() {
		return yearOf(epochTime());
	}

	static int yearOf(unsigned long epochTime) {
		return year(epochTime);
	}

	byte monthOf() {
		return monthOf(epochTime());
	}

	static byte monthOf(unsigned long epochTime) {
		//month from 1 to 12
		return month(epochTime);
	}

	byte dayOf() {
		return dayOf(epochTime());
	}

	static byte dayOf(unsigned long epochTime) {
		//day from 1 to 31
		return day(epochTime);
	}

	static bool isTimeLaterThan(byte epochTimeHours, byte epochTimeMinutes, byte hours, byte minutes) {
		return ((epochTimeHours > hours) || ((epochTimeHours == hours) && (epochTimeMinutes >= minutes)));
	}

	bool isTimeLaterThan(byte hours, byte minutes) {
		return isTimeLaterThan(this->hours(), this->minutes(), hours, minutes);
	}

	bool isTimeEarlierThan(byte hours, byte minutes) {
		return ((this->hours() < hours) || ((this->hours() == hours) && (this->minutes() < minutes)));
	}

	bool isTimeBetween(byte fromHours, byte fromMinutes, byte toHours, byte toMinutes) {
		if (isTimeLaterThan(fromHours, fromMinutes, toHours, toMinutes)) {
			//e.g. 22:00-06:00
			return ((isTimeLaterThan(fromHours, fromMinutes)) || (isTimeEarlierThan(toHours, toMinutes)));
		} else {
			//e.g. 06:00-22:00
			return ((isTimeLaterThan(fromHours, fromMinutes)) && (isTimeEarlierThan(toHours, toMinutes)));
		}
	}

	void updateFormattedTime() {
		WStringStream* stream = updateFormattedTime(epochTime());
		_epochTimeFormatted->asString(stream->c_str());
		delete stream;
	}

	static WStringStream* updateFormattedTime(unsigned long rawTime) {
		// Format YY-MM-DD hh:mm:ss
		WStringStream* stream = new WStringStream(19);
		char buffer[20];
		snprintf(buffer, 20, "%02d-%02d-%02d %02d:%02d:%02d",
		                     yearOf(rawTime), monthOf(rawTime), dayOf(rawTime),
												 ((rawTime % 86400L) / 3600), ((rawTime % 3600) / 60), rawTime % 60);
		stream->print(buffer);
		return stream;
	}

	bool isValidTime() {
		return validTime->asBool();
	}

	bool isClockSynced() {
		return isValidTime();
	}

	void printConfigPage(AsyncWebServerRequest* request, Print* page) {
    	page->printf(HTTP_CONFIG_PAGE_BEGIN, id());
			if (this->enableNightMode) {
				page->printf(HTTP_TOGGLE_GROUP_STYLE, "gn", (enableNightMode->asBool() ? HTTP_BLOCK : HTTP_NONE), "gm", HTTP_NONE);
			}
			//NTP Server
			page->printf(HTTP_TEXT_FIELD, "NTP server:", "ntp", "32", ntpServer->c_str());
			//Time zone
			byte preset = timeZonePreset(timeZone->c_str());
			page->printf(HTTP_COMBOBOX_BEGIN, "Time zone:", "tzp");
			for (byte i = 0; i < TIME_ZONE_PRESET_COUNT; i++) {
				page->printf(HTTP_COMBOBOX_ITEM, String(i).c_str(), (preset == i ? HTTP_SELECTED : ""), TIME_ZONE_PRESETS[i].name);
			}
			page->printf(HTTP_COMBOBOX_ITEM, String(TIME_ZONE_PRESET_CUSTOM).c_str(), (preset == TIME_ZONE_PRESET_CUSTOM ? HTTP_SELECTED : ""), "Custom (POSIX time zone below)");
			page->print(FPSTR(HTTP_COMBOBOX_END));
			page->printf(HTTP_TEXT_FIELD, "Custom POSIX time zone, e.g. 'CET-1CEST,M3.5.0,M10.5.0/3':", "tzc", "63",
			             (preset == TIME_ZONE_PRESET_CUSTOM ? timeZone->c_str() : ""));
			if (this->enableNightMode) {
				//nightMode
				page->printf(HTTP_CHECKBOX_OPTION, "sn", "sn", (enableNightMode->asBool() ? HTTP_CHECKED : ""), "tn()", "Enable support for night mode");
				page->printf(HTTP_DIV_ID_BEGIN, "gn");
				page->print(F("<table  class='settingstable'>"));
					page->print(F("<tr>"));
						char timeFrom[6];
						snprintf(timeFrom, 6, "%02d:%02d", this->nightSwitches->byteArrayValue(0), this->nightSwitches->byteArrayValue(1));
						page->print(F("<td>from"));
						page->printf(HTTP_INPUT_FIELD, "nf", "5", timeFrom);
						page->print(F("</td>"));
						char timeTo[6];
						snprintf(timeTo, 6, "%02d:%02d", this->nightSwitches->byteArrayValue(2), this->nightSwitches->byteArrayValue(3));
						page->print(F("<td>to"));
						page->printf(HTTP_INPUT_FIELD, "nt", "5", timeTo);
						page->print(F("</td>"));
					page->print(F("</tr>"));
				page->print(F("</table>"));
				page->print(FPSTR(HTTP_DIV_END));
				page->printf(HTTP_TOGGLE_FUNCTION_SCRIPT, "tn()", "sn", "gn", "gm");
			}
    	page->print(FPSTR(HTTP_CONFIG_SAVE_BUTTON));
	}

	void saveConfigPage(AsyncWebServerRequest* request, Print* page) {
		this->ntpServer->asString(request->arg("ntp").c_str());
		int preset = request->arg("tzp").toInt();
		if (preset < TIME_ZONE_PRESET_COUNT) {
			this->timeZone->asString(TIME_ZONE_PRESETS[preset].posix);
		} else {
			String custom = request->arg("tzc");
			custom.trim();
			if (custom.length() > 0) {
				this->timeZone->asString(custom.c_str());
			}
		}
		if (this->enableNightMode) {
			this->enableNightMode->asBool(request->arg("sn") == HTTP_TRUE);
			processNightModeTime(0, request->arg("nf").c_str());
			processNightModeTime(2, request->arg("nt").c_str());
		}
		if (timeConfigured) {
			configureTime();
		}
	}

	WProperty* epochTimeFormatted() {
		return _epochTimeFormatted;
	}

	WProperty* nightMode;

private:
	THandlerFunction _onTimeUpdate, _onMinuteTrigger;
	unsigned long lastTry;
	bool timeConfigured;
	volatile bool timeSynced;
	int lastDst;
	//SNTP keeps the pointer to the server name
	char ntpServerName[33];
	WProperty* _epochTimeFormatted;
	WProperty* validTime;
	WProperty* ntpServer;
	WProperty* useTimeZoneServer;
	WProperty* timeZone;
	WProperty* rawOffset;
	WProperty* dstOffset;
	WProperty* useDaySavingTimes;
	WProperty* dstRule;
	WProperty* enableNightMode;
	WProperty* nightSwitches;

	void _notifyOnTimeUpdate() {
		if (_onTimeUpdate) {
			_onTimeUpdate();
		}
		_notifyOnMinuteUpdate();
	}

	void _notifyOnMinuteUpdate() {
		if (_onMinuteTrigger) {
			_onMinuteTrigger();
		}
	}

	void configureTime() {
		strncpy(ntpServerName, ntpServer->c_str(), sizeof(ntpServerName) - 1);
		ntpServerName[sizeof(ntpServerName) - 1] = '\0';
		network()->debug(F("Time via NTP server '%s', time zone '%s'"), ntpServerName, timeZone->c_str());
		//SNTP resyncs hourly, day saving time is handled by the time zone rules
		configTime(timeZone->c_str(), ntpServerName);
		timeConfigured = true;
	}

	struct tm localTime() {
		time_t now = time(nullptr);
		struct tm lt;
		localtime_r(&now, &lt);
		return lt;
	}

	static byte timeZonePreset(const char* tz) {
		for (byte i = 0; i < TIME_ZONE_PRESET_COUNT; i++) {
			if (strcmp(TIME_ZONE_PRESETS[i].posix, tz) == 0) {
				return i;
			}
		}
		return TIME_ZONE_PRESET_CUSTOM;
	}

	void migrateTimeZone() {
		//Settings of versions before 1.38.3 contain the time zone server URL here
		if ((!timeZone->isNull()) && (strlen(timeZone->c_str()) > 0) && (strstr(timeZone->c_str(), "://") == nullptr)) {
			return;
		}
		String tz = migratedTimeZone();
		timeZone->asString(tz.c_str());
		network()->debug(F("Time zone migrated to '%s'"), tz.c_str());
	}

	String migratedTimeZone() {
		int raw = rawOffset->asInt();
		bool euRule = true;
		bool dst;
		if (useTimeZoneServer->asBool()) {
			//Only the last raw offset is known, assume EU rule in european time zones
			dst = ((raw == 0) || (raw == 3600) || (raw == 7200));
		} else {
			dst = useDaySavingTimes->asBool();
			for (byte i = 0; i < 8; i++) {
				euRule = euRule && (dstRule->byteArrayValue(i) == DEFAULT_DST_RULE[i]);
			}
		}
		if ((dst) && (euRule)) {
			switch (raw) {
				case 0: return TIME_ZONE_PRESETS[1].posix;
				case 3600: return TIME_ZONE_PRESETS[0].posix;
				case 7200: return TIME_ZONE_PRESETS[3].posix;
			}
		}
		String result = posixName(raw) + posixOffset(raw);
		if (dst) {
			int dstRaw = raw + dstOffset->asInt();
			result = result + posixName(dstRaw) + posixOffset(dstRaw) +
			         posixRule(DST_MONTH, DST_WEEK, DST_WEEKDAY, DST_HOUR) +
			         posixRule(STD_MONTH, STD_WEEK, STD_WEEKDAY, STD_HOUR);
		}
		return result;
	}

	static String posixName(int offsetSeconds) {
		//e.g. 3600 -> '<+01>', -12600 -> '<-0330>'
		int m = abs(offsetSeconds) / 60;
		char buffer[10];
		if (m % 60 == 0) {
			snprintf(buffer, sizeof(buffer), "<%c%02d>", (offsetSeconds < 0 ? '-' : '+'), m / 60);
		} else {
			snprintf(buffer, sizeof(buffer), "<%c%02d%02d>", (offsetSeconds < 0 ? '-' : '+'), m / 60, m % 60);
		}
		return String(buffer);
	}

	static String posixOffset(int offsetSeconds) {
		//POSIX offsets are west of UTC: 3600 -> '-1', -12600 -> '3:30'
		int m = abs(offsetSeconds) / 60;
		String result = (offsetSeconds > 0 ? "-" : "") + String(m / 60);
		if (m % 60 != 0) {
			char buffer[4];
			snprintf(buffer, sizeof(buffer), ":%02d", m % 60);
			result += buffer;
		}
		return result;
	}

	String posixRule(byte month, byte week, byte weekday, byte hour) {
		//week 0 is last week of month, POSIX uses 5
		byte w = dstRule->byteArrayValue(week);
		return ",M" + String(dstRule->byteArrayValue(month)) + "." + String(w == 0 ? 5 : w) + "." +
		       String(dstRule->byteArrayValue(weekday)) + "/" + String(dstRule->byteArrayValue(hour));
	}

	void processNightModeTime(byte arrayIndex, String timeStr) {
		timeStr = (timeStr.length() == 4 ? "0" + timeStr : timeStr);
		if (timeStr.length() == 5) {
			byte hh = timeStr.substring(0, 2).toInt();
			byte mm = timeStr.substring(3, 5).toInt();
			this->nightSwitches->byteArrayValue(arrayIndex, hh);
			this->nightSwitches->byteArrayValue(arrayIndex + 1, mm);
		}
	}

};

#endif
