# Trill: Touch Sensing for Makers (esp-idf port)

Trill sensors bring beautiful touch interaction to digital projects. Trill was [funded on Kickstarter in 2019](https://www.kickstarter.com/projects/423153472/trill-touch-sensing-for-makers), and Trill sensors are available now in the [Bela Shop](https://shop.bela.io/collections/trill).

## Trill Libraries and Examples

The repository holds the Arduino library for Trill, as well as code examples. ([Bela and Linux library and examples are located here](https://github.com/BelaPlatform/Trill).)

Visit [https://learn.bela.io/trill](https://learn.bela.io/trill) for full documentation and a Get Started guide.

## esp-idf port
It's a bit hacky and only parts that I myself needed are ported, so some functions are just commented out.
Usage same as for Arduino, except I2C_PORT needs to be initialized and then passed to the begin function:
```
/* Init I2C */
i2c_config_t i2c_config;
i2c_config.mode = I2C_MODE_MASTER;
i2c_config.sda_io_num = SDA_PIN;
i2c_config.sda_pullup_en = GPIO_PULLUP_ENABLE;
i2c_config.scl_io_num = SCL_PIN;
i2c_config.scl_pullup_en = GPIO_PULLUP_ENABLE;
i2c_config.master.clk_speed = 100000;

ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_config));
ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

int ret = trillSensor.setup(Trill::TRILL_SQUARE, I2C_NUM_0);
printf("Trill Setup: %d\n", ret);
bool touchActive = false;
while (running) {
    trillSensor.read();
    if (trillSensor.getNumTouches() > 0 && trillSensor.getNumHorizontalTouches() > 0) {
        if (!touchActive) {
            printf("START:");
        }

        printf("%d", trillSensor.touchHorizontalLocation(0));
        printf(",");

        printf("%d", 1792 - trillSensor.touchLocation(0));
        printf(",");

        touchActive = true;
    }
    else if (touchActive) {
        // Print a single line when touch goes off
        touchActive = false;
        printf("\n");
    }
    vTaskDelay(pdMS_TO_TICKS(2));
}
```