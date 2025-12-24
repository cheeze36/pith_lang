@echo off
SETLOCAL ENABLEDELAYEDEXPANSION

REM --- Configuration ---
SET EXECUTABLE=cmake-build-debug\pith_lang.exe
SET TEST_DIR=tests
SET PASS_COUNT=0
SET FAIL_COUNT=0

SET TEST_NAMES=^
    test_arithmetic ^
    test_control_flow ^
    test_debug ^
    test_functions ^
    test_hashmaps ^
    test_lists ^
    test_lists_extended ^
    test_literals ^
    test_math ^
    test_string_stdlib ^
    test_variables ^
    classtest ^
    test_classes ^
    test_arrays ^
    test_elif ^
    test_all_features ^
    test_stdlib ^
    test_foreach_loop ^
    test_for_c_style ^
    test_multiline_comments ^
    test_io

ECHO.
ECHO ============================
ECHO      Running Pith Tests
ECHO ============================
ECHO.

FOR %%T IN (%TEST_NAMES%) DO (
    SET TEST_NAME=%%T
    SET TEST_FILE=!TEST_DIR!\!TEST_NAME!.pith
    SET EXPECTED_FILE=!TEST_DIR!\!TEST_NAME!.expected
    SET ACTUAL_FILE=!TEST_DIR!\!TEST_NAME!.actual

    ECHO Running test: !TEST_NAME!

    REM Run the interpreter and save the output
    !EXECUTABLE! !TEST_FILE! > !ACTUAL_FILE!

    REM Compare the actual output with the expected output
    FC "!ACTUAL_FILE!" "!EXPECTED_FILE!" > nul
    IF !ERRORLEVEL! EQU 0 (
        ECHO   - PASS
        SET /A PASS_COUNT+=1
    ) ELSE (
        ECHO   - FAIL
        ECHO     Expected:
        TYPE "!EXPECTED_FILE!"
        ECHO.
        ECHO     Actual:
        TYPE "!ACTUAL_FILE!"
        ECHO.
        SET /A FAIL_COUNT+=1
    )

    REM Clean up the temporary actual file
    DEL "!ACTUAL_FILE!"
)

ECHO.
ECHO ============================
ECHO        Test Summary
ECHO ============================
ECHO Passed: !PASS_COUNT!
ECHO Failed: !FAIL_COUNT!
ECHO.

IF !FAIL_COUNT! GTR 0 (
    EXIT /B 1
) ELSE (
    EXIT /B 0
)
