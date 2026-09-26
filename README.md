# Far Cry — Nintendo Switch Port

Nintendo Switch port of **Far Cry** based on the Android/Linux version of **NearChuckle**.

The port runs through **VNX Translation Core**, with **Mesa** providing the graphics stack and **SDL3** handling the input/window integration.

## Installation

Create the following directory on your Switch SD card:

```text
/switch/NearChuckle_nx/
```

Put the NRO in that directory and create a `game` folder:

```text
/switch/NearChuckle_nx/
├── NearChuckle_nx.nro
└── game/
```

Copy these **four folders** from your Far Cry game files into `game/`:

```text
/switch/NearChuckle_nx/game/
├── fcdata/
├── languages/
├── levels/
└── profiles/
```

The final layout should look like:

```text
/switch/NearChuckle_nx/
├── NearChuckle_nx.nro
└── game/
    ├── fcdata/
    ├── languages/
    ├── levels/
    └── profiles/
```

Launch `NearChuckle_nx.nro` from the Homebrew Menu.

## Performance

A **performance overclock is recommended**.

The game can take a while to start and load on a stock Switch. Using an overclock can significantly reduce startup and loading times and makes the initial loading process smoother.

## Controls

| Switch control | In-game action |
|---|---|
| **A** | Enter / Confirm |
| **B** | Jump |
| **R** | Run |
| **X** | Reload |
| **Y** | Use / Interact |
| **D-Pad Left** | Previous weapon |
| **D-Pad Right** | Next weapon |
| **D-Pad Down** | Prone |
| **D-Pad Up** | Night Vision |
| **ZL** | Aim |
| **ZR** | Fire |
| **Right Stick Click** | Flashlight |
| **Left Stick** | Move |
| **Right Stick** | Look |

## Known Issues

### Textures in the starting bunker

There are currently **texture rendering issues in the starting bunker**. Some textures may appear missing, corrupted, or incorrectly rendered.

This is a known issue with the current port.

## Mod Support

**Mod support has not been tested yet.**

Mods may or may not work depending on how they access Far Cry's original files and engine systems.

## Building

To build the Switch port, the source tree requires the **Mesa** and **SDL3** components used by the project.

The project is built with the **devkitPro / devkitA64** toolchain.

Typical build:

```text
make
```

The resulting `NearChuckle_nx.nro` can then be copied to the Switch SD card.

## Credits

- **NaGaa95** — Mesa / Switch graphics work
- **NearChuckle** — Linux port
- **Viridite** — [VNX Translation Core](https://github.com/Viridite/VNX-Translation-Core)

## Disclaimer

This project is a compatibility/porting project. You must provide your own legally obtained Far Cry game data.
