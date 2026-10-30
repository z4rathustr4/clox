# Clox

### My implementation of the language showcased in the "Crafting Interpreters" book by Robert Nystrom

- You can buy it here: https://craftinginterpreters.com/


Note: if for some godforsaken reason you prefer using CMake (yes, I thought it
would be a good idea myself), you can generate `compile_commands.json` using

```console
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1
```

and then build it:
```console
mkdir build && cd build && cmake ..
```
