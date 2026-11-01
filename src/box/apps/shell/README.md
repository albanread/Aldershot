# Shell

The shell is the part that every ROSAPPS application shares. It provides the document window, the toolbar, the rulers, the tool palette, the inspector, the properties pane, the menus, the save box and Export, printing, the clipboard, the question boxes an engine asks for, and the Choices. It keeps more than one document open at a time, each in its own window with its own engine, or as tabs in one window. Each application is the shell with its own settings, so they all look and work the same way.

`rosapp.mojo` is the shell itself. `Shell.mojo` is !Shell, an empty application used to try the shell out with a stub engine. The other applications each import `rosapp.mojo`.

The shell is written in Mojo. The document is handled by an engine, a separate Rust program, which the shell talks to through the ROSApps module. The engines are not published here.

## Licence

MIT. Each source file names its licence in its header.
