# S3-Amysynth






### Prototype

[Prototype](https://github.com/user-attachments/assets/a7d58778-f2d6-4067-9d59-5099f2a55ba3)

---





## Overview

**S3-Amysynth** is an embedded audio project built on the **ESP32-S3** using **ESP-IDF**. It combines firmware development, real-time audio generation, hardware interfacing, and UI design in a custom handheld synthesizer platform.

The project is designed to demonstrate practical embedded engineering skills in a form that is easy to assess at a glance: what the system does, how it is built, and what technical areas it develops.



## What the Project Does

S3-Amysynth is a microcontroller-based synthesizer and drum sequencer with a hardware interface and live audio output.

### Core functionality

- Real-time audio generation using the **AMY synth engine**
- **16-step drum sequencer** with editable playback
- **Dynamic Melodic Layers** Additional Melodic layers can be added and removed
- **OLED user interface** for sequencer state and controls
- **Rotary encoder** and **buttons** for navigation and tempo/input control
- **USB Audio Class 2.0** audio streaming to a PC or DAW during development
- **I2S DAC/Amp** for on device audio planed in later stages
- Support for a custom ESP32-S3 hardware platform with dedicated audio and display peripherals



## Technical Stack

### Hardware

- **ESP32-S3-N16R8**
- **PCM5102 I2S DAC**
- **SSD1306 128×64 OLED**
- Rotary encoder
- Push buttons

### Software

- **ESP-IDF 6.0**
- **FreeRTOS**
- **TinyUSB**
- **AMY synth engine**
- **U8g2** graphics library
- Embedded **C**



## Skills Demonstrated

This project reflects development in the following areas:

### Embedded Firmware Development

- Writing structured firmware for a microcontroller platform
- Initializing and coordinating multiple peripherals
- Working within hardware and memory constraints

### Real-Time Systems

- Managing timing-sensitive behavior for audio and sequencing
- Coordinating tasks across a dual-core embedded platform
- Handling responsiveness requirements for user input and playback

### Audio Systems

- Implementing digital audio behavior on constrained hardware
- Working with sample-rate-sensitive output paths
- Understanding the relationship between synthesis, buffering, and playback timing

### Hardware-Software Integration

- Interfacing with display, buttons, encoder, ADC input, and audio hardware
- Debugging issues that cross between firmware behavior and physical hardware
- Building software around a custom board design rather than a generic dev-board workflow

### Debugging and Engineering Workflow

- Diagnosing timing, configuration, and integration issues
- Applying targeted fixes to system-level problems
- Documenting technical decisions and implementation details



## Knowledge Level Indicated

This project brings together several overlapping areas of interest, including embedded firmware, real-time systems, digital audio, hardware interfacing, and UX design. As a self-directed project outside formal coursework, it reflects strong motivation, a willingness to take on difficult technical challenges, and a passion for the field. Relative to roughly six months of embedded development experience, the scope is purposefully ambitious and challenging. 
This project hopefully indicates progress beyond beginner exercises toward more advanced systems-level work.

### Strongest indicators

- Comfortable working in an **ESP-IDF** environment
- Practical understanding of **microcontroller peripherals**
- Exposure to **real-time audio and task-based firmware design**
- Ability to integrate multiple subsystems into a functioning embedded application
- Growing familiarity with debugging issues involving timing, concurrency, and external interfaces

### What this suggests

The project goes beyond introductory firmware exercises and shows progression toward more capable work in:
- embedded systems
- firmware engineering
- real-time software
- audio-oriented embedded development



## Engineering Challenges Addressed

The project includes work in areas that commonly appear in real embedded development:

- Real-time sequencing and playback timing
- USB audio output integration
- Embedded UI design on a small display
- Input handling with shared control modes
- Audio pipeline configuration and synchronization
- Multi-component firmware organization

These are useful indicators of hands-on engineering ability because they require more than isolated feature implementation.

## Open-Source Dependency Practice

This project depends on several open-source components, including **AMY**, **U8g2**, **TinyUSB**, and ESP-IDF-managed libraries. Development is approached with a clear separation between **project-owned code** and **external dependencies**.

### Approach

- Project behavior is implemented in local application and component code first
- External libraries are treated as stable upstream sources, not as the default place to make changes
- Upstream code is only edited when incorrect behavior is clearly confirmed and a local-layer fix is not sufficient
- Any external patch is kept minimal, documented, and tracked separately from project code
- Internal and external edits are recorded distinctly to make maintenance, review, and rollback easier
- Confirmed upstream issues are intended to be reported back to the original source where appropriate



## Why This Project Matters

S3-Amysynth is a good example of systems-level development because it combines:

- low-level firmware work
- hardware interaction
- audio processing concepts
- user interface logic
- debugging across multiple subsystems

It communicates both technical curiosity and the ability to carry a project across design, implementation, testing, and refinement.



## Current Focus

The project is currently focused on:

- improving embedded audio reliability
- refining sequencer behavior and controls
- strengthening hardware-software integration
- building a clearer and more maintainable firmware structure



## Summary

S3-Amysynth is a custom **ESP32-S3** embedded audio system built with **ESP-IDF 6.0**, **FreeRTOS**, and the **AMY** synthesis engine. It combines real-time sound generation, a live-editable **16-step drum sequencer**, an **SSD1306 OLED** interface, and hardware input handling through a **rotary encoder** and push buttons on a custom handheld platform.

The current firmware renders audio on the microcontroller and streams it over **USB Audio Class 2.0** using **TinyUSB** at **48 kHz stereo 16-bit**, while the hardware design also includes an **I2S PCM5102 DAC** path for later standalone output. The system brings together multiple embedded concerns in one project: task coordination across a dual-core MCU, timing-sensitive sequencing, audio buffering, peripheral integration, and UI feedback on a constrained device.

Overall, the project is a practical example of systems-level embedded development that spans:
- firmware architecture
- real-time audio processing
- hardware-software integration
- user input and display management
- debugging across project-owned code and upstream dependencies
