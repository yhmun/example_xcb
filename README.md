## XCB Example

This contains useful xcb example snippets.

## Prerequisites

* install python3 and ninja

    ```
    sudo apt install python3 python3-pip python3-setuptools python3-wheel ninja-build
    ```
* install meson

    * install as a local user (recommended)
        ```
        pip3 install --user meson
        ```
    * intall as root
        ```
        sudo pip3 install meson
        ```

## Build

* setup
    ```
    meson setup <src_root> <src_root>/build
    ```
* compile
    ```
    meson compile <src_root>/build
    ```

## Showcases

* xcb_info

    this shows how to read xcb_connection and xcb_screen information

* xcb_atom

    this demonstrates reading and caching xcb_atom values

* xcb_signal

    this demonstrates how to handle unix signal-safe

    - Press Ctrl+C
