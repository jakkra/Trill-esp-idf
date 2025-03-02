/*
 * Trill library for Arduino
 * (c) 2020 bela.io
 *
 * This library communicates with the Trill sensors
 * using I2C.
 *
 * BSD license
 */

#include "Trill.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// some implementations of Wire (e.g.: Particle OS) do not define BUFFER_LENGTH
#ifndef BUFFER_LENGTH
#define BUFFER_LENGTH 32
#endif // BUFFER_LENGTH

#define MAX_TOUCH_1D_OR_2D (((device_type_ == TRILL_SQUARE || device_type_ == TRILL_HEX) ? kMaxTouchNum2D : kMaxTouchNum1D))
#define RAW_LENGTH ((device_type_ == TRILL_BAR ? 2 * kNumChannelsBar \
			: device_type_ == TRILL_RING ? 2 * kNumChannelsRing \
			: 2 * kNumChannelsMax))

Trill::Trill()
: device_type_(TRILL_NONE), firmware_version_(0),
  mode_(AUTO), last_read_loc_(0xFF), raw_bytes_left_(0)
{
}

/* Initialise the hardware. Returns the type of device attached, or 0
   if none is attached. i2c_port must be initialized before calling this function.
*/
int Trill::begin(Device device, i2c_port_t i2c_port, uint8_t i2c_address) {

	if(128 <= i2c_address)
		i2c_address = trillDefaults[device+1].address;

	/* Unknown default address */
	if(128 <= i2c_address) {
		return  -2;
	}

	i2c_address_ = i2c_address;
	i2c_port_ = i2c_port;

	/* Check the type of device attached */
	if(identify() != 0) {
		// Unable to identify device
		return 2;
	}

	/* Check for wrong device type */
	if(TRILL_UNKNOWN != device && device_type_ != device) {
		device_type_ = TRILL_NONE;
		return -3;
	}

	/* Check for device mode */
	Mode mode = trillDefaults[device+1].mode;
	if(AUTO == mode) {
		return -1;
	}

	/* Put the device in the correspondent mode */
	setMode(mode);
	vTaskDelay(pdMS_TO_TICKS(interCommandDelay));

	Touches::centroids = buffer_;
	Touches::sizes = buffer_ + MAX_TOUCH_1D_OR_2D;
	if(is2D()) {
		horizontal.centroids = buffer_ + 2 * MAX_TOUCH_1D_OR_2D;
		horizontal.sizes = buffer_ + 3 * MAX_TOUCH_1D_OR_2D;
	} else
		horizontal.num_touches = 0;

	/* Set default scan settings */
	setScanSettings(0, 12);
	vTaskDelay(pdMS_TO_TICKS(interCommandDelay));

	updateBaseline();
	vTaskDelay(pdMS_TO_TICKS(interCommandDelay)); // not really needed, but it ensures the first command the user sends after calling setup() will be adequately timed. Hopefully this is not a source of confusion...	
	return 0;
}

/* Return the type of device attached, or 0 if none is attached. */
int Trill::identify() {
	esp_err_t ret;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, i2c_address_ << 1 | I2C_MASTER_WRITE, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kOffsetCommand, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kCommandIdentify, 1));
	ret = i2c_master_cmd_begin(i2c_port_, cmd, pdMS_TO_TICKS(50)); // send the i2c command
  	i2c_cmd_link_delete(cmd);

	if (ret != ESP_OK) {
		return 0;
	}
	/* Give Trill time to process this command */
	vTaskDelay(pdMS_TO_TICKS(25));

	last_read_loc_ = kOffsetCommand;
	//Wire.requestFrom(i2c_address_, (uint8_t)3);
	cmd = i2c_cmd_link_create();
	uint8_t length = 3;
	uint8_t buff[length];
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (i2c_address_ << 1) | I2C_MASTER_READ, 1));
	ESP_ERROR_CHECK(i2c_master_read(cmd, buff, length - 1, I2C_MASTER_ACK));
	ESP_ERROR_CHECK(i2c_master_read_byte(cmd, buff + length - 1, I2C_MASTER_NACK));
	ESP_ERROR_CHECK(i2c_master_stop(cmd));
	ret = i2c_master_cmd_begin(i2c_port_, cmd, 50 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);

	if (ret != ESP_OK) {
		device_type_ = TRILL_NONE;
		firmware_version_ = 0;
		return -1;
	}
	
	device_type_ = (Device)buff[1];
	firmware_version_ = buff[2];

	return 0;
}

/* Read the latest scan value from the sensor. Returns true on success. */
bool Trill::read() {
	if(CENTROID != mode_)
		return false;
	uint8_t loc = 0;
	uint8_t length = kCentroidLengthDefault;
	esp_err_t err;

	/* Set the read location to the right place if needed */
	prepareForDataRead();

	if(device_type_ == TRILL_SQUARE || device_type_ == TRILL_HEX)
		length = kCentroidLength2D;

	if(device_type_ == TRILL_RING)
		length = kCentroidLengthRing;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	uint8_t buff[length];
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (i2c_address_ << 1) | I2C_MASTER_READ, 1));
	ESP_ERROR_CHECK(i2c_master_read(cmd, buff, length - 1, I2C_MASTER_ACK));
	ESP_ERROR_CHECK(i2c_master_read_byte(cmd, buff + length - 1, I2C_MASTER_NACK));
	ESP_ERROR_CHECK(i2c_master_stop(cmd));
	err = i2c_master_cmd_begin(i2c_port_, cmd, 50 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);

	if (err != ESP_OK) {
		return false;
	}

	for (int i = 0; i < length; i+=2) {
		uint8_t msb = buff[i];
		uint8_t lsb = buff[i + 1];
		buffer_[loc] = lsb + (msb << 8);
		++loc;
	}

	uint8_t maxNumCentroids = MAX_TOUCH_1D_OR_2D;
	bool ret = true;
	/* Check for read error */
	if(loc * 2 < length) {
		maxNumCentroids = 0;
		ret = false;
	}

	processCentroids(maxNumCentroids);
	if(is2D())
		horizontal.processCentroids(maxNumCentroids);

	return ret;
}

/* Update the baseline value on the sensor */
void Trill::updateBaseline() {
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, i2c_address_ << 1 | I2C_MASTER_WRITE, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kOffsetCommand, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kCommandBaselineUpdate, 1));
	ESP_ERROR_CHECK(i2c_master_cmd_begin(i2c_port_, cmd, pdMS_TO_TICKS(50)));
  	i2c_cmd_link_delete(cmd);

	last_read_loc_ = kOffsetCommand;
}
//
///* Request raw data; wrappers for Wire */
//bool Trill::requestRawData(uint8_t max_length) {
//	uint8_t length = 0;
//
//	prepareForDataRead();
//
//	if(max_length == 0xFF) {
//		length = RAW_LENGTH;
//	}
//	if(length > kRawLength)
//		length = kRawLength;
//
//	/* The raw data might be longer than the Wire.h maximum buffer
//	 * (BUFFER_LENGTH in Wire.h).
//	 * If so, split it into two reads. */
//	if(length <= BUFFER_LENGTH) {
//		Wire.requestFrom(i2c_address_, length);
//		raw_bytes_left_ = 0;
//	}
//	else {
//		int ret = Wire.requestFrom(i2c_address_, (uint8_t)BUFFER_LENGTH);
//		if(ret > 0)
//			raw_bytes_left_ = length - ret;
//		else {
//			// failed transmission. Device died?
//			raw_bytes_left_ = 0;
//			return false;
//		}
//	}
//	return true;
//}
//
//int Trill::rawDataAvailable() {
//	/* Raw data items are 2 bytes long; return number of them available */
//	return ((Wire.available() + raw_bytes_left_) >> 1);
//}
//
///* Raw data is in 16-bit big-endian format */
//int Trill::rawDataRead() {
//
//	if(Wire.available() < 2) {
//		/* Read more bytes if we need it */
//		if(raw_bytes_left_ > 0) {
//			/* Move read pointer on device */
//			Wire.beginTransmission(i2c_address_);
//			Wire.write(kOffsetData + BUFFER_LENGTH);
//			Wire.endTransmission();
//			last_read_loc_ = kOffsetData + BUFFER_LENGTH;
//
//			/* Now gather what's left */
//			Wire.requestFrom(i2c_address_, raw_bytes_left_);
//			raw_bytes_left_ = 0;
//		}
//
//		/* Check again if we've got anything... */
//		if(Wire.available() < 2)
//			return 0;
//	}
//
//	int result = ((uint8_t)Wire.read()) << 8;
//	result += (int)Wire.read();
//	return result;
//}

/* Scan configuration settings */
void Trill::setMode(Mode mode) {
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, i2c_address_ << 1 | I2C_MASTER_WRITE, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kOffsetCommand, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kCommandMode, 1));
	ESP_ERROR_CHECK(i2c_master_cmd_begin(i2c_port_, cmd, pdMS_TO_TICKS(50)));
  	i2c_cmd_link_delete(cmd);

	mode_ = mode;
	last_read_loc_ = kOffsetCommand;
	num_touches = 0;
}

void Trill::setScanSettings(uint8_t speed, uint8_t num_bits) {
	if(speed > 3)
		speed = 3;
	if(num_bits < 9)
		num_bits = 9;
	if(num_bits > 16)
		num_bits = 16;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	ESP_ERROR_CHECK(i2c_master_start(cmd));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, i2c_address_ << 1 | I2C_MASTER_WRITE, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kOffsetCommand, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kCommandScanSettings, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, speed, 1));
	ESP_ERROR_CHECK(i2c_master_write_byte(cmd, num_bits, 1));
	ESP_ERROR_CHECK(i2c_master_cmd_begin(i2c_port_, cmd, pdMS_TO_TICKS(50)));
  	i2c_cmd_link_delete(cmd);

	last_read_loc_ = kOffsetCommand;
}

//void Trill::setPrescaler(uint8_t prescaler) {
//	Wire.beginTransmission(i2c_address_);
//	Wire.write(kOffsetCommand);
//	Wire.write(kCommandPrescaler);
//	Wire.write(prescaler);
//	Wire.endTransmission();
//
//	last_read_loc_ = kOffsetCommand;
//}
//
//void Trill::setNoiseThreshold(uint8_t threshold) {
//	if(threshold > 255)
//		threshold = 255;
//	if(threshold < 0)
//		threshold = 0;
//	Wire.beginTransmission(i2c_address_);
//	Wire.write(kOffsetCommand);
//	Wire.write(kCommandNoiseThreshold);
//	Wire.write(threshold);
//	Wire.endTransmission();
//
//	last_read_loc_ = kOffsetCommand;
//}
//
//void Trill::setIDACValue(uint8_t value) {
//	Wire.beginTransmission(i2c_address_);
//	Wire.write(kOffsetCommand);
//	Wire.write(kCommandIdac);
//	Wire.write(value);
//	Wire.endTransmission();
//
//	last_read_loc_ = kOffsetCommand;
//}
//
//void Trill::setMinimumTouchSize(uint16_t size) {
//	Wire.beginTransmission(i2c_address_);
//	Wire.write(kOffsetCommand);
//	Wire.write(kCommandMinimumSize);
//	Wire.write(size >> 8);
//	Wire.write(size & 0xFF);
//	Wire.endTransmission();
//
//	last_read_loc_ = kOffsetCommand;
//}
//
//void Trill::setAutoScanInterval(uint16_t interval) {
//	Wire.beginTransmission(i2c_address_);
//	Wire.write(kOffsetCommand);
//	Wire.write(kCommandAutoScanInterval);
//	Wire.write(interval >> 8);
//	Wire.write(interval & 0xFF);
//	Wire.endTransmission();
//
//	last_read_loc_ = kOffsetCommand;
//}

/* Prepare the device to read data if it is not already prepared */
void Trill::prepareForDataRead() {
	if(last_read_loc_ != kOffsetData) {
		i2c_cmd_handle_t cmd = i2c_cmd_link_create();
		ESP_ERROR_CHECK(i2c_master_start(cmd));
		ESP_ERROR_CHECK(i2c_master_write_byte(cmd, i2c_address_ << 1 | I2C_MASTER_WRITE, 1));
		ESP_ERROR_CHECK(i2c_master_write_byte(cmd, kOffsetData, 1));
		ESP_ERROR_CHECK(i2c_master_cmd_begin(i2c_port_, cmd, pdMS_TO_TICKS(50)));
		i2c_cmd_link_delete(cmd);

		last_read_loc_ = kOffsetData;
	}
}

int Trill::getButtonValue(uint8_t button_num)
{
	if(mode_ != CENTROID)
		return -1;
	if(button_num > 1)
		return -1;
	if(device_type_ != TRILL_RING)
		return -1;

	return buffer_[2 * MAX_TOUCH_1D_OR_2D + button_num];
}

unsigned int Trill::getNumChannels()
{
	switch(device_type_) {
		case TRILL_BAR: return kNumChannelsBar;
		case TRILL_RING: return kNumChannelsRing;
		default: return kNumChannelsMax;
	}
}

bool Trill::is1D()
{
	if(CENTROID != mode_)
		return false;
	switch(device_type_) {
		case TRILL_BAR:
		case TRILL_RING:
		case TRILL_CRAFT:
		case TRILL_FLEX:
			return true;
		default:
			return false;
	}
}

bool Trill::is2D()
{
	if(CENTROID != mode_)
		return false;
	switch(device_type_) {
		case TRILL_SQUARE:
		case TRILL_HEX:
			return true;
		default:
			return false;
	}
}

uint8_t Touches::getNumTouches() const
{
	return num_touches;
}

int Touches::touchLocation(uint8_t touch_num) const
{
	if(touch_num < num_touches)
		return centroids[touch_num];
	else
		return -1;
}

int Touches::touchSize(uint8_t touch_num) const
{
	if(touch_num < num_touches)
		return sizes[touch_num];
	else
		return -1;
}

unsigned int Touches2D::getNumHorizontalTouches() {
	return horizontal.getNumTouches();
}

void Touches::processCentroids(uint8_t maxCentroids) {
	// Look for 1st instance of 0xFFFF (no touch) in the buffer
	for(num_touches = 0; num_touches < maxCentroids; ++num_touches)
	{
		if(0xffff == centroids[num_touches])
			break;// at the first non-touch, break
	}
	// now num_touches is the number of active touches in the array
}

/* These methods for horizontal touches on 2D sliders */
int Touches2D::touchHorizontalLocation(uint8_t touch_num) {
	return horizontal.touchLocation(touch_num);
}

int Touches2D::touchHorizontalSize(uint8_t touch_num) {
	return horizontal.touchSize(touch_num);
}
