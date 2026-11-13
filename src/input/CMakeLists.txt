add_library(CemuInput
	InputManager.cpp
	InputManager.h
	ControllerFactory.cpp
	ControllerFactory.h
	api/ControllerState.h
	api/Controller.cpp
	api/Controller.h
	api/ControllerState.cpp
	api/InputAPI.h
	api/ControllerProvider.h
	api/DSU/DSUController.h
	api/DSU/DSUControllerProvider.cpp
	api/DSU/DSUController.cpp
	api/DSU/DSUControllerProvider.h
	api/DSU/DSUMessages.h
	api/DSU/DSUMessages.cpp
	api/RemoteGamePad/RemoteGamePadProtocol.h
	api/RemoteGamePad/RemoteGamePadProtocol.cpp
	api/RemoteGamePad/RemoteGamePadControllerProvider.h
	api/RemoteGamePad/RemoteGamePadControllerProvider.cpp
	api/RemoteGamePad/RemoteGamePadController.h
	api/RemoteGamePad/RemoteGamePadController.cpp
	api/RemoteGamePad/RemoteGamePadTransport.h
	api/RemoteGamePad/RemoteGamePadTransportUDP.cpp
	api/RemoteGamePad/RemoteGamePadMedia.h
	api/RemoteGamePad/RemoteGamePadMediaCore.h
	api/RemoteGamePad/RemoteGamePadMediaCore.cpp
	api/Keyboard/KeyboardControllerProvider.h
	api/Keyboard/KeyboardControllerProvider.cpp
	api/Keyboard/KeyboardController.cpp
	api/Keyboard/KeyboardController.h
	emulated/ProController.cpp
	emulated/EmulatedController.h
	emulated/EmulatedController.cpp
	emulated/ProController.h
	emulated/WPADController.cpp
	emulated/WPADController.h
	emulated/WiimoteController.h
	emulated/VPADController.cpp
	emulated/WiimoteController.cpp
	emulated/VPADController.h
	emulated/ClassicController.cpp
	emulated/ClassicController.h
	emulated/BalanceBoardController.cpp
	emulated/BalanceBoardController.h
	emulated/BalanceBoardMath.cpp
	emulated/BalanceBoardMath.h
)

set_property(TARGET CemuInput PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuInput)

# Remote GamePad media (video encoder, phone camera and mic): VideoToolbox on macOS, stubs elsewhere
if(APPLE)
	target_sources(CemuInput PRIVATE api/RemoteGamePad/RemoteGamePadMediaApple.cpp)
	target_link_libraries(CemuInput PRIVATE
		"-framework VideoToolbox"
		"-framework CoreMedia"
		"-framework CoreVideo"
		"-framework IOSurface"
		"-framework CoreFoundation"
	)
else()
	target_sources(CemuInput PRIVATE api/RemoteGamePad/RemoteGamePadMediaNull.cpp)
endif()
# UDP with reply-from-the-request's-address (IP_PKTINFO); its own feature macros, so no PCH
if(NOT WIN32)
	target_sources(CemuInput PRIVATE api/RemoteGamePad/RemoteGamePadUdp.h api/RemoteGamePad/RemoteGamePadUdp.cpp)
	set_source_files_properties(api/RemoteGamePad/RemoteGamePadUdp.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

if (ENABLE_LIBUSB)
	target_sources(CemuInput PRIVATE
		api/GameCube/GameCubeController.cpp
		api/GameCube/GameCubeControllerProvider.h
		api/GameCube/GameCubeControllerProvider.cpp
		api/GameCube/GameCubeController.h
	)
endif()

if(WIN32)
	# XInput
	target_sources(CemuInput PRIVATE
		api/XInput/XInputControllerProvider.cpp
		api/XInput/XInputControllerProvider.h
		api/XInput/XInputController.cpp
		api/XInput/XInputController.h
	)

	# DirectInput
	target_sources(CemuInput PRIVATE
		api/DirectInput/DirectInputControllerProvider.cpp
		api/DirectInput/DirectInputController.h
		api/DirectInput/DirectInputControllerProvider.h
		api/DirectInput/DirectInputController.cpp
	)
endif()

if (SUPPORTS_WIIMOTE)
	target_compile_definitions(CemuInput PUBLIC SUPPORTS_WIIMOTE)
	target_sources(CemuInput PRIVATE
			api/Wiimote/WiimoteControllerProvider.h
			api/Wiimote/WiimoteControllerProvider.cpp
			api/Wiimote/WiimoteMessages.h
			api/Wiimote/NativeWiimoteController.h
			api/Wiimote/NativeWiimoteController.cpp
			api/Wiimote/WiimoteDevice.h
			api/Wiimote/bridge/WiiBridgeWiimote.h
			api/Wiimote/bridge/WiiBridgeWiimote.cpp
	)
	if (ENABLE_HIDAPI)
		target_sources(CemuInput PRIVATE
				api/Wiimote/hidapi/HidapiWiimote.cpp
				api/Wiimote/hidapi/HidapiWiimote.h)
	endif ()

	if (ENABLE_BLUEZ)
		target_sources(CemuInput PRIVATE
				api/Wiimote/l2cap/L2CapWiimote.cpp
				api/Wiimote/l2cap/L2CapWiimote.h)
	endif()

	if (APPLE AND ENABLE_IOBLUETOOTH)
		target_sources(CemuInput PRIVATE
				api/Wiimote/darwin/IOBluetoothWiimote.mm
				api/Wiimote/darwin/IOBluetoothWiimote.h)
		target_link_libraries(CemuInput PRIVATE "-framework IOBluetooth" "-framework CoreBluetooth" "-framework Foundation")

		# standalone test: scan / connect / print the first reports, outside Cemu
		add_executable(wiimote_bt_test
				api/Wiimote/darwin/wiimote_bt_test.mm
				api/Wiimote/darwin/IOBluetoothWiimote.mm)
		target_compile_definitions(wiimote_bt_test PRIVATE IOBT_STANDALONE)
		target_include_directories(wiimote_bt_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
		target_compile_features(wiimote_bt_test PRIVATE cxx_std_20)
		# an embedded Info.plist with the Bluetooth usage description, for the permission prompt
		target_link_options(wiimote_bt_test PRIVATE
				"LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/api/Wiimote/darwin/wiimote_bt_test-Info.plist")
		target_link_libraries(wiimote_bt_test PRIVATE "-framework IOBluetooth" "-framework CoreBluetooth" "-framework Foundation")
		set_target_properties(wiimote_bt_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
	endif()
endif ()


if(ENABLE_SDL)
	target_sources(CemuInput PRIVATE
		api/SDL/SDLController.cpp
		api/SDL/SDLControllerProvider.cpp
		api/SDL/SDLController.h
		api/SDL/SDLControllerProvider.h
	)
	target_link_libraries(CemuInput PRIVATE	SDL3::SDL3)
endif()

target_include_directories(CemuInput PUBLIC "../")

target_link_libraries(CemuInput PRIVATE
	CemuCommon
	CemuGui
)

if (ENABLE_BLUEZ)
	target_link_libraries(CemuInput PRIVATE bluez::bluez)
endif ()
