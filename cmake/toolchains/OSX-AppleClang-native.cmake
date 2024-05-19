INCLUDE("${CMAKE_CURRENT_LIST_DIR}/include/OSX-AppleClang_common.inc.cmake")

function (toolchain_after_project)
	MESSAGE (STATUS "Toolchain: OSX-AppleClang-native.cmake.")
	SET(CMAKE_SYSTEM_NAME	"OSX"		PARENT_SCOPE)

	IF (CMAKE_SIZEOF_VOID_P EQUAL 8)
		MESSAGE (STATUS "Detected 64 bits architecture")
		SET(ARCH_BITS	64	PARENT_SCOPE)
		SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin-native64"	PARENT_SCOPE)
	ELSE (CMAKE_SIZEOF_VOID_P EQUAL 8)
		MESSAGE (STATUS "Detected 32 bits architecture")
		SET(ARCH_BITS	32	PARENT_SCOPE)
		SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin-native32"	PARENT_SCOPE)
	ENDIF (CMAKE_SIZEOF_VOID_P EQUAL 8)
endfunction()


function (toolchain_exe_stuff)
	SET (C_ARCH_OPTS	"-march=native")
	SET (CXX_ARCH_OPTS	"-march=native")

	toolchain_exe_stuff_common()

	# Propagate global variables set in toolchain_exe_stuff_common to the upper scope
	SET (CMAKE_C_FLAGS			"${CMAKE_C_FLAGS} ${C_ARCH_OPTS}"       PARENT_SCOPE)
	SET (CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} ${CXX_ARCH_OPTS}"	PARENT_SCOPE)
	SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}" 			PARENT_SCOPE)
endfunction()
