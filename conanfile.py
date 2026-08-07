from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class ShmringConan(ConanFile):
    name = "shmring"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("catch2/3.8.1")
        self.requires("benchmark/1.9.5")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()
