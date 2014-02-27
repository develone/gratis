// Copyright 2013 Pervasive Displays, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at:
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied.  See the License for the specific language
// governing permissions and limitations under the License.


#include <Arduino.h>
#include <limits.h>

#include <SPI.h>

#include "EPD2.h"

// delays - more consistent naming
#define Delay_ms(ms) delay(ms)
#define Delay_us(us) delayMicroseconds(us)

// inline arrays
#define ARRAY(type, ...) ((type[]){__VA_ARGS__})
#define CU8(...) (ARRAY(const uint8_t, __VA_ARGS__))


static void SPI_on();
static void SPI_off();
static void SPI_put(uint8_t c);
static void SPI_put_wait(uint8_t c, int busy_pin);
static void SPI_send(uint8_t cs_pin, const uint8_t *buffer, uint16_t length);
static uint8_t SPI_read(uint8_t cs_pin, const uint8_t *buffer, uint16_t length);


EPD_Class::EPD_Class(EPD_size size,
		     int panel_on_pin,
		     int border_pin,
		     int discharge_pin,
		     int reset_pin,
		     int busy_pin,
		     int chip_select_pin) :
	EPD_Pin_PANEL_ON(panel_on_pin),
	EPD_Pin_BORDER(border_pin),
	EPD_Pin_DISCHARGE(discharge_pin),
	EPD_Pin_RESET(reset_pin),
	EPD_Pin_BUSY(busy_pin),
	EPD_Pin_EPD_CS(chip_select_pin) {

	this->size = size;
	this->lines_per_display = 96;
	this->dots_per_line = 128;
	this->bytes_per_line = 128 / 8;
	this->bytes_per_scan = 96 / 4;

	this->setFactor(); // ensure default temperature

	// display size dependant items
	{
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x00};
		static uint8_t gs[] = {0x72, 0x03};
		this->channel_select = cs;
		this->channel_select_length = sizeof(cs);
	}

	// set up size structure
	switch (size) {
	default:
	case EPD_1_44:  // default so no change
		break;

	case EPD_2_0: {
		this->lines_per_display = 96;
		this->dots_per_line = 200;
		this->bytes_per_line = 200 / 8;
		this->bytes_per_scan = 96 / 4;
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xe0, 0x00};
		this->channel_select = cs;
		this->channel_select_length = sizeof(cs);
		break;
	}

	case EPD_2_7: {
		this->lines_per_display = 176;
		this->dots_per_line = 264;
		this->bytes_per_line = 264 / 8;
		this->bytes_per_scan = 176 / 4;
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x00, 0x00};
		this->channel_select = cs;
		this->channel_select_length = sizeof(cs);
		break;
	}
	}

}


void  EPD_Class::setFactor(int temperature) {
	static const EPD_Class::compensation_type compensation_144[3] = {
		{ 2, 6, 42,   4, 392, 392,   2, 6, 42 },  //  0 ... 10 Celcius
		{ 4, 2, 16,   4, 155, 155,   4, 2, 16 },  // 10 ... 40 Celcius
		{ 4, 2, 16,   4, 155, 155,   4, 2, 16 }   // 40 ... 50 Celcius
	};

	static const EPD_Class::compensation_type compensation_200[3] = {
		{ 2, 6, 42,   4, 392, 392,   2, 6, 42 },  //  0 ... 10 Celcius
		{ 2, 2, 48,   4, 196, 196,   2, 2, 48 },  // 10 ... 40 Celcius
		{ 4, 2, 48,   4, 196, 196,   4, 2, 48 }   // 40 ... 50 Celcius
	};

	static const EPD_Class::compensation_type compensation_270[3] = {
		{ 2, 8, 64,   4, 392, 392,   2, 8, 64 },  //  0 ... 10 Celcius
		{ 2, 8, 64,   4, 196, 196,   2, 8, 64 },  // 10 ... 40 Celcius
		{ 4, 8, 64,   4, 196, 196,   4, 8, 64 }   // 40 ... 50 Celcius
	};

	if (temperature < 10) {
		this->temperature_offset = 0;
	} else if (temperature > 40) {
		this->temperature_offset = 2;
	} else {
		this->temperature_offset = 1;
	}
	switch (this->size) {
	default:
	case EPD_1_44:
		this->compensation = &compensation_144[this->temperature_offset];
		break;

	case EPD_2_0: {
		this->compensation = &compensation_200[this->temperature_offset];
		break;
	}

	case EPD_2_7: {
		this->compensation = &compensation_270[this->temperature_offset];
		break;
	}
	}
}


void EPD_Class::begin() {

	// assume ok
	this->status = EPD_OK;

	// power up sequence
	digitalWrite(this->EPD_Pin_RESET, LOW);
	digitalWrite(this->EPD_Pin_PANEL_ON, LOW);
	digitalWrite(this->EPD_Pin_DISCHARGE, LOW);
	digitalWrite(this->EPD_Pin_BORDER, LOW);
	digitalWrite(this->EPD_Pin_EPD_CS, LOW);

	SPI_on();

	Delay_ms(5);
	digitalWrite(this->EPD_Pin_PANEL_ON, HIGH);
	Delay_ms(10);

	digitalWrite(this->EPD_Pin_RESET, HIGH);
	digitalWrite(this->EPD_Pin_BORDER, HIGH);
	digitalWrite(this->EPD_Pin_EPD_CS, HIGH);
	Delay_ms(5);

	digitalWrite(this->EPD_Pin_RESET, LOW);
	Delay_ms(5);

	digitalWrite(this->EPD_Pin_RESET, HIGH);
	Delay_ms(5);

	// wait for COG to become ready
	while (HIGH == digitalRead(this->EPD_Pin_BUSY)) {
		Delay_us(10);
	}

	// read the COG ID
	int cog_id = SPI_read(this->EPD_Pin_EPD_CS, CU8(0x71, 0x00), 2);
	cog_id = SPI_read(this->EPD_Pin_EPD_CS, CU8(0x71, 0x00), 2);

	if (0x02 != (0x0f & cog_id)) {
		this->status = EPD_UNSUPPORTED_COG;
		this->power_off();
		return;
	}

	// Disable OE
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x02), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x40), 2);

	// check breakage
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x0f), 2);
	int broken_panel = SPI_read(this->EPD_Pin_EPD_CS, CU8(0x73, 0x00), 2);
	if (0x00 == (0x80 & broken_panel)) {
		this->status = EPD_PANEL_BROKEN;
		this->power_off();
		return;
	}

	// power saving mode
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x0b), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x02), 2);

	// channel select
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x01), 2);
	SPI_send(this->EPD_Pin_EPD_CS, this->channel_select, this->channel_select_length);

	// high power mode osc
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x07), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0xd1), 2);

	// power setting
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x08), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x02), 2);

	// Vcom level
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x09), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0xc2), 2);

	// power setting
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x04), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x03), 2);

	// driver latch on
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x03), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x01), 2);

	// driver latch off
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x03), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x00), 2);

	Delay_ms(5);

	bool dc_ok = false;

	for (int i = 0; i < 4; ++i) {
		// charge pump positive voltage on - VGH/VDL on
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x01), 2);

		Delay_ms(240);

		// charge pump negative voltage on - VGL/VDL on
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x03), 2);

		Delay_ms(40);

		// charge pump Vcom on - Vcom driver on
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x0f), 2);

		Delay_ms(40);

		// check DC/DC
		SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x0f), 2);
		int dc_state = SPI_read(this->EPD_Pin_EPD_CS, CU8(0x73, 0x00), 2);
		if (0x40 == (0x40 & dc_state)) {
			dc_ok = true;
			break;
		}
	}
	if (!dc_ok) {
		this->status = EPD_DC_FAILED;
		this->power_off();
		return;
	}

	// output enable to disable
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x02), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x40), 2);

	SPI_off();
}


void EPD_Class::end() {

	// dummy line and border
	if (EPD_2_7 == this->size) {
		// only for 2.70" EPD
		Delay_ms(25);
		digitalWrite(this->EPD_Pin_BORDER, LOW);
		Delay_ms(250);
		digitalWrite(this->EPD_Pin_BORDER, HIGH);

	} else {

		// for 2.00"
		this->line(0x7fffu, 0, 0x00, false, EPD_normal, 0xff);
		Delay_ms(40);
		this->line(0x7fffu, 0, 0x00, false, EPD_normal, 0xaa);
		Delay_ms(200);
		this->line(0x7fffu, 0, 0x00, false, EPD_normal);
		Delay_ms(25);
	}

	SPI_on();

	// check DC/DC
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x0f), 2);
	int dc_state = SPI_read(this->EPD_Pin_EPD_CS, CU8(0x73, 0x00), 2);
	if (0x40 != (0x40 & dc_state)) {
		this->status = EPD_DC_FAILED;
		this->power_off();
		return;
	}

	// latch reset turn on
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x03), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x01), 2);

	// output enable off
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x02), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x05), 2);

	// power off positive charge pump
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x0e), 2);

	// power off Vcom charge pump
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x02), 2);

	// power off all charge pumps
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x05), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x00), 2);

	// turn of osc
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x07), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x0d), 2);

	// discharge internal on
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x04), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x83), 2);

	Delay_ms(120);

	// discharge internal off
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x04), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x00), 2);

	power_off();
}

void EPD_Class::power_off() {
	// turn of power and all signals
	digitalWrite(this->EPD_Pin_RESET, LOW);
	digitalWrite(this->EPD_Pin_PANEL_ON, LOW);
	digitalWrite(this->EPD_Pin_BORDER, LOW);

	// ensure SPI MOSI and CLOCK are Low before CS Low
	SPI_off();
	digitalWrite(this->EPD_Pin_EPD_CS, LOW);

	// pulse discharge pin
	for (int i = 0; i < 10; ++i) {
		Delay_ms(10);
		digitalWrite(this->EPD_Pin_DISCHARGE, HIGH);
		Delay_ms(10);
		digitalWrite(this->EPD_Pin_DISCHARGE, LOW);
	}
}


// One frame of data is the number of lines * rows. For example:
// The 1.44” frame of data is 96 lines * 128 dots.
// The 2” frame of data is 96 lines * 200 dots.
// The 2.7” frame of data is 176 lines * 264 dots.

// the image is arranged by line which matches the display size
// so smallest would have 96 * 32 bytes

void EPD_Class::frame_fixed_timed(uint8_t fixed_value, long stage_time) {
	do {
		unsigned long t_start = millis();
		for (uint8_t line = 0; line < this->lines_per_display ; ++line) {
			this->line(line, 0, fixed_value, false);
		}
		unsigned long t_end = millis();
		if (t_end > t_start) {
			stage_time -= t_end - t_start;
		} else {
			stage_time -= t_start - t_end + 1 + ULONG_MAX;
		}
	} while (stage_time > 0);
}



void EPD_Class::frame_fixed_13(uint8_t value, EPD_stage stage) {

	int repeat;
	int step;
	int block;
	if (EPD_inverse == stage) {  // stage 1
		repeat = this->compensation->stage1_repeat;
		step = this->compensation->stage1_step;
		block = this->compensation->stage1_block;
	} else {                     // stage 3
		repeat = this->compensation->stage3_repeat;
		step = this->compensation->stage3_step;
		block = this->compensation->stage3_block;
	}

	int total_lines = this->lines_per_display;

	for (int n = 0; n < repeat; ++n) {

		for (int line = step - block; line < total_lines + step; line += step) {
			for (int offset = 0; offset < block; ++offset) {
				int pos = line + offset;
				if (pos < 0 || pos > total_lines) {
					this->line(0x7fffu, 0, 0x00, false, EPD_normal);
				} else if (0 == offset && n == repeat - 1) {
					this->line(pos, 0, 0x00, false, EPD_normal);
				} else {
					this->line(pos, 0, value, false, stage);
				}
			}
		}
	}
}


void EPD_Class::frame_data_13(PROGMEM const uint8_t *image, EPD_stage stage) {

	int repeat;
	int step;
	int block;
	if (EPD_inverse == stage) {  // stage 1
		repeat = this->compensation->stage1_repeat;
		step = this->compensation->stage1_step;
		block = this->compensation->stage1_block;
	} else {                     // stage 3
		repeat = this->compensation->stage3_repeat;
		step = this->compensation->stage3_step;
		block = this->compensation->stage3_block;
	}

	int total_lines = this->lines_per_display;

	for (int n = 0; n < repeat; ++n) {

		for (int line = step - block; line < total_lines + step; line += step) {
			for (int offset = 0; offset < block; ++offset) {
				int pos = line + offset;
				if (pos < 0 || pos > total_lines) {
					this->line(0x7fffu, 0, 0x00, false, EPD_normal);
				} else if (0 == offset && n == repeat - 1) {
					this->line(pos, 0, 0x00, false, EPD_normal);
				} else {
					this->line(pos, &image[pos * this->bytes_per_line], 0, true, stage);
				}
			}
		}
	}
}

#if 0
void EPD_Class::frame_data(PROGMEM const uint8_t *image, EPD_stage stage) {
	for (uint8_t line = 0; line < this->lines_per_display ; ++line) {
		this->line(line, &image[line * this->bytes_per_line], 0, true, stage);
	}
}
#endif


#if defined(EPD_ENABLE_EXTRA_SRAM)
void EPD_Class::frame_sram(const uint8_t *image, EPD_stage stage) {
	for (uint8_t line = 0; line < this->lines_per_display ; ++line) {
		this->line(line, &image[line * this->bytes_per_line], 0, false, stage);
	}
}
#endif

#if 0
void EPD_Class::frame_cb(uint32_t address, EPD_reader *reader, EPD_stage stage) {
	static uint8_t buffer[264 / 8];
	for (uint8_t line = 0; line < this->lines_per_display; ++line) {
		reader(buffer, address + line * this->bytes_per_line, this->bytes_per_line);
		this->line(line, buffer, 0, false, stage);
	}
}
#endif

void EPD_Class::frame_stage2() {
	for (int i = 0; i < this->compensation->stage2_repeat; ++i) {
		this->frame_fixed_timed(0xff, this->compensation->stage2_t1);
		this->frame_fixed_timed(0xaa, this->compensation->stage2_t2);
	}
}

#if 0
void EPD_Class::frame_fixed_repeat(uint8_t fixed_value, EPD_stage stage) {
	int repeat = (EPD_inverse == stage) ? this->compensation->stage1_repeat : this->compensation->stage3_repeat;
	for (int i = 0; i < repeat; ++i) {
		this->frame_fixed(fixed_value);
	}
}

void EPD_Class::frame_data_repeat(PROGMEM const uint8_t *image, EPD_stage stage) {
	int repeat = (EPD_inverse == stage) ? this->compensation->stage1_repeat : this->compensation->stage3_repeat;
	for (int i = 0; i < repeat; ++i) {
		this->frame_data(image, stage);
	}
}
#endif

#if defined(EPD_ENABLE_EXTRA_SRAM)
void EPD_Class::frame_sram_repeat(const uint8_t *image, EPD_stage stage) {
	long stage_time = this->factored_stage_time;
	do {
		unsigned long t_start = millis();
		this->frame_sram(image, stage);
		unsigned long t_end = millis();
		if (t_end > t_start) {
			stage_time -= t_end - t_start;
		} else {
			stage_time -= t_start - t_end + 1 + ULONG_MAX;
		}
	} while (stage_time > 0);
}
#endif


#if 0
void EPD_Class::frame_cb_repeat(uint32_t address, EPD_reader *reader, EPD_stage stage) {
	int repeat = (EPD_inverse == stage) ? this->compensation->stage1_repeat : this->compensation->stage3_repeat;
	for (int i = 0; i < repeat; ++i) {
		this->frame_cb(address, reader, stage);
	}
}
#endif

void EPD_Class::line(uint16_t line, const uint8_t *data, uint8_t fixed_value,
		     bool read_progmem, EPD_stage stage, uint8_t border_byte) {

	SPI_on();

	// send data
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x0a), 2);

	Delay_us(10);

	// CS low
	digitalWrite(this->EPD_Pin_EPD_CS, LOW);
	SPI_put_wait(0x72, this->EPD_Pin_BUSY);

	// border byte
	SPI_put_wait(border_byte, this->EPD_Pin_BUSY);

	// odd pixels
	for (uint16_t b = this->bytes_per_line; b > 0; --b) {
		if (0 != data) {
#if defined(__MSP430_CPU__)
			uint8_t pixels = data[b - 1] & 0x55;
#else
			// AVR has multiple memory spaces
			uint8_t pixels;
			if (read_progmem) {
				pixels = pgm_read_byte_near(data + b - 1) & 0x55;
			} else {
				pixels = data[b - 1] & 0x55;
			}
#endif
			switch(stage) {
			case EPD_inverse:      // B -> W, W -> B
				pixels = 0xaa | (pixels ^ 0x55);
				break;
			case EPD_normal:       // B -> B, W -> W
				pixels = 0xaa | pixels;
				break;
			}
			SPI_put_wait(pixels, this->EPD_Pin_BUSY);
		} else {
			SPI_put_wait(fixed_value, this->EPD_Pin_BUSY);
		}
	}

	// scan line
	int scan_pos = (this->lines_per_display - line - 1) / 4;
	int scan_shift = 2 * (line & 0x03);
	for (uint16_t b = 0; b < this->bytes_per_scan; ++b) {
		if (scan_pos == b) {
			SPI_put_wait(0x03 << scan_shift, this->EPD_Pin_BUSY);
		} else {
			SPI_put_wait(0x00, this->EPD_Pin_BUSY);
		}
	}

	// even pixels
	for (uint16_t b = 0; b < this->bytes_per_line; ++b) {
		if (0 != data) {
#if defined(__MSP430_CPU__)
			uint8_t pixels = data[b] & 0xaa;
#else
			// AVR has multiple memory spaces
			uint8_t pixels;
			if (read_progmem) {
				pixels = pgm_read_byte_near(data + b) & 0xaa;
			} else {
				pixels = data[b] & 0xaa;
			}
#endif
			switch(stage) {
			case EPD_inverse:      // B -> W, W -> B (Current Image)
				pixels = 0xaa | ((pixels ^ 0xaa) >> 1);
				break;
			case EPD_normal:       // B -> B, W -> W (New Image)
				pixels = 0xaa | (pixels >> 1);
				break;
			}
			uint8_t p1 = (pixels >> 6) & 0x03;
			uint8_t p2 = (pixels >> 4) & 0x03;
			uint8_t p3 = (pixels >> 2) & 0x03;
			uint8_t p4 = (pixels >> 0) & 0x03;
			pixels = (p1 << 0) | (p2 << 2) | (p3 << 4) | (p4 << 6);
			SPI_put_wait(pixels, this->EPD_Pin_BUSY);
		} else {
			SPI_put_wait(fixed_value, this->EPD_Pin_BUSY);
		}
	}

	// CS high
	digitalWrite(this->EPD_Pin_EPD_CS, HIGH);

	// output data to panel
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x70, 0x02), 2);
	SPI_send(this->EPD_Pin_EPD_CS, CU8(0x72, 0x07), 2);

	SPI_off();
}


static void SPI_on() {
	SPI.end();
	SPI.begin();
	SPI.setBitOrder(MSBFIRST);
	SPI.setDataMode(SPI_MODE0);
	SPI.setClockDivider(SPI_CLOCK_DIV2);
	SPI_put(0x00);
	SPI_put(0x00);
	Delay_us(10);
}


static void SPI_off() {
	// SPI.begin();
	// SPI.setBitOrder(MSBFIRST);
	SPI.setDataMode(SPI_MODE0);
	// SPI.setClockDivider(SPI_CLOCK_DIV2);
	SPI_put(0x00);
	SPI_put(0x00);
	Delay_us(10);
	SPI.end();
}


static void SPI_put(uint8_t c) {
	SPI.transfer(c);
}


static void SPI_put_wait(uint8_t c, int busy_pin) {

	SPI_put(c);

	// wait for COG ready
	while (HIGH == digitalRead(busy_pin)) {
	}
}


static void SPI_send(uint8_t cs_pin, const uint8_t *buffer, uint16_t length) {
	// CS low
	Delay_us(10);
	digitalWrite(cs_pin, LOW);

	// send all data
	for (uint16_t i = 0; i < length; ++i) {
		SPI_put(*buffer++);
	}

	// CS high
	digitalWrite(cs_pin, HIGH);
}

static uint8_t SPI_read(uint8_t cs_pin, const uint8_t *buffer, uint16_t length) {
	// CS low
	Delay_us(10);
	digitalWrite(cs_pin, LOW);

	uint8_t rbuffer[4];
	int i = 0;
	uint8_t result = 0;

	// send all data
	for (uint16_t i = 0; i < length; ++i) {
		result = SPI.transfer(*buffer++);
		if (i < 4) {
			rbuffer[i] = result;
		}
	}

	// CS high
	digitalWrite(cs_pin, HIGH);
	return result;
}
