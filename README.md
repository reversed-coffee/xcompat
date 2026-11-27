# xcompat

An experimental Xbox Series X|S (Scarlett) compatibility layer for Windows.

xcompat aims to enable native execution of Scarlett binaries on Windows by providing a
compatible runtime environment and reimplementing Xbox-specific system interfaces.

## Current state

The project is in the early stages of research and prototyping.

Currently implemented:

- Native NT guest process creation
- PE image mapping using native NT image sections

## Architecture

Since Scarlett binaries are packaged in a PE32+ format, the binaries are relatively
compatible with Windows. This allows xcompat to load the binary in memory space similar
to how Windows loads an executable.

xcompat starts by creating a guest process in Windows and maps the Scarlett executable
into its memory space. The host process is responsible for managing the guest process, loading modules, applying relocations, and resolving imported symbols.


![Loader Architecture](./docs/assets/xcompat-loader.drawio.svg)

Where possible, imported symbols are resolved directly to native Windows APIs. Unresolved
or incompatible interfaces are instead routed through the xcompat runtime, which provides
implementations of Scarlett-specific APIs and translates platform-specific behavior into
equivalent Windows functionality.

Since Scarlett is targeted to run on an x86-64 processor, guest code can execute natively
without CPU emulation. This allows xcompat to focus primarily on compatibility at the
operating system, runtime, and hardware interface levels.

## Development

xcompat is written in modern C++ and builds upon hydra for low-level systems programming,
memory management, and PE manipulation.

This project is currently a proof of concept and is nowhere near capable of running
titles.

## Disclaimer

xcompat is an independent project and is not affiliated with or endorsed by Microsoft or
Xbox.
