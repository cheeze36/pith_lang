cmake_minimum_required(VERSION 3.28)
project(pith_lang)

set(CMAKE_C_STANDARD 99)

add_executable(pith_lang main.c tokenizer.c parser.c interpreter.c repl.c gc.c)
