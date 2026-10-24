.RECIPEPREFIX := >

CC := gcc
CXX := g++

APP_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
APP_CFLAGS += -Iinclude
APP_CFLAGS += $(shell pkg-config --cflags libmicrohttpd json-c libmosquitto libcurl sqlite3)

APP_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -g
APP_CXXFLAGS += -Iinclude
APP_CXXFLAGS += $(shell pkg-config --cflags opencv4)

APP_LDLIBS := $(shell pkg-config --libs libmicrohttpd json-c libmosquitto libcurl sqlite3)
APP_LDLIBS += $(shell pkg-config --libs opencv4)
APP_LDLIBS += -pthread

MQTT_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
MQTT_CFLAGS += $(shell pkg-config --cflags libmosquitto json-c libcurl)

MQTT_LDLIBS := $(shell pkg-config --libs libmosquitto json-c libcurl)
MQTT_LDLIBS += -pthread

WATCHDOG_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
WATCHDOG_CFLAGS += $(shell pkg-config --cflags libcurl)

WATCHDOG_LDLIBS := $(shell pkg-config --libs libcurl)

APP_TARGET := bin/embedded_monitor
MQTT_TARGET := bin/smart_guard_mqtt
WATCHDOG_TARGET := bin/smart_guard_watchdog

APP_C_SOURCES := $(wildcard src/*.c)
APP_CPP_SOURCES := $(wildcard image_processing/*.cpp)

APP_C_OBJECTS := $(patsubst src/%.c,build/%.o,$(APP_C_SOURCES))
APP_CPP_OBJECTS := $(patsubst image_processing/%.cpp,build/image_processing/%.o,$(APP_CPP_SOURCES))

MQTT_OBJECT := build/mqtt/mqtt_client.o
WATCHDOG_OBJECT := build/watchdog/watchdog.o

.PHONY: all clean run

all: $(APP_TARGET) $(MQTT_TARGET) $(WATCHDOG_TARGET)

$(APP_TARGET): $(APP_C_OBJECTS) $(APP_CPP_OBJECTS)
>mkdir -p bin
>$(CXX) $(APP_C_OBJECTS) $(APP_CPP_OBJECTS) -o $(APP_TARGET) $(APP_LDLIBS)

$(MQTT_TARGET): $(MQTT_OBJECT)
>mkdir -p bin
>$(CC) $(MQTT_OBJECT) -o $(MQTT_TARGET) $(MQTT_LDLIBS)

$(WATCHDOG_TARGET): $(WATCHDOG_OBJECT)
>mkdir -p bin
>$(CC) $(WATCHDOG_OBJECT) -o $(WATCHDOG_TARGET) $(WATCHDOG_LDLIBS)

build/%.o: src/%.c
>mkdir -p $(dir $@)
>$(CC) $(APP_CFLAGS) -c $< -o $@

build/image_processing/%.o: image_processing/%.cpp
>mkdir -p $(dir $@)
>$(CXX) $(APP_CXXFLAGS) -c $< -o $@

build/mqtt/%.o: mqtt/%.c
>mkdir -p $(dir $@)
>$(CC) $(MQTT_CFLAGS) -c $< -o $@

build/watchdog/%.o: watchdog/%.c
>mkdir -p $(dir $@)
>$(CC) $(WATCHDOG_CFLAGS) -c $< -o $@

run: $(APP_TARGET)
>./$(APP_TARGET)

clean:
>rm -rf build $(APP_TARGET) $(MQTT_TARGET) $(WATCHDOG_TARGET)
