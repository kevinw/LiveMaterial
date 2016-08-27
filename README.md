# LiveMaterial

A plugin allowing for updating .shader files live, even in standalone builds.

## The problem

Unity's shader pipeline assumes you have all your .shader files written at build time. But what if you want to write code to generate .shader files dynamically? This works while the editor is running, but not in a standalone build.

## How it works

LiveMaterial is a Unity rendering plugin that implements much of the same interface as Material, but accepts new shader text at runtime.

## Current limitations

- Hardcoded vertices assume you're only rendering a single full-screen quad, so it's not a generalized Material replacement
- D3D11 and OpenGL only
