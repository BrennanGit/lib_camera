set(LIB_NAME fwk_camera) #TODO change name
set(LIB_VERSION 0.2.1)
set(LIB_DEPENDENT_MODULES i2c )
set(LIB_INCLUDES api src/sensors/_sony_imx219)
set(LIB_COMPILER_FLAGS -Os -Wall -Werror -g -fxscope -mcmodel=large)

XMOS_REGISTER_MODULE()
