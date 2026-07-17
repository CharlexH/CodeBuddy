import importlib

__all__ = ["__version__", "runtime", "shell_integration", "shim", "setup_flow"]

__version__ = "0.1.13"


def __getattr__(name):
    if name in __all__ and name != "__version__":
        module = importlib.import_module(f".{name}", __name__)
        globals()[name] = module
        return module
    raise AttributeError(name)
