# xcompat

An experimental Xbox Series X|S (Scarlett) compatibility layer for Windows.

xcompat aims to enable native execution of Scarlett binaries on Windows by providing a
compatible runtime environment and reimplementing Xbox-specific system interfaces.

## Current state

The project is in the early stages of research and prototyping.

Currently implemented:

- Native NT guest process creation
- PE image mapping using native NT image sections

## Development

xcompat is written in modern C++ and builds upon hydra for low-level systems programming,
memory management, and PE manipulation.

This project is currently a proof of concept and is nowhere near capable of running
titles.

## Disclaimer

xcompat is an independent project and is not affiliated with or endorsed by Microsoft or
Xbox.
