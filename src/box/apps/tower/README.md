# Tower

!Tower is a tower-building game. Its engine runs the tower simulation on the box's Linux side, a translation of tower-together's MIT simulation, and the graphics card draws the tower where the box's presenter can. The tool palette has every room the game has; the properties pane shows what a tool costs and needs, the room inspected, the finances and the evaluation. Towers are saved as JSON (type &A9D).

`Tower.mojo` is the front end. It needs the shared shell, `../shell/rosapp.mojo`. The engine and the simulation are published in [box-tower](https://github.com/albanread/box-tower).

## Licence

MIT. Each source file names its licence in its header.
