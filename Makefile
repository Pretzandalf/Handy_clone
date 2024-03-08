.SILENT:
.ONESHELL:
# https://stackoverflow.com/questions/2131213#comment133331794_60363121
.RECIPEPREFIX := $(.RECIPEPREFIX) $(.RECIPEPREFIX)

SHELL = /bin/bash
CMAKE_BUILD_TYPE ?= Release
CMAKE_TOOLS_ADDRESS_SANITIZER ?= OFF
CXXFLAGS := \
    ${CXXFLAGS} \
    $(shell if [ "$${CMAKE_TOOLS_ADDRESS_SANITIZER^^}" = "ON" ]; then echo "-fsanitize=address"; fi)
CMAKE_ARGS ?= \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}"

FILES_TO_LINT := $(shell find . \( -name "*.h" -or -name "*.cpp" -or -name "*.cc" \) \
                    -not -path "*/build/*" -not -path "*/install/*" -not -path "*/log/*")

.PHONY: all
all:
    $(error Please use explicit targets)

.PHONY: build-all
build-all:
    source ${ROS_ROOT}/setup.sh
    colcon --log-base /dev/null build \
        --base-paths packages \
        --symlink-install \
        --cmake-args ${CMAKE_ARGS}

.PHONY: test-all
test-all:
    source ${ROS_ROOT}/setup.sh
    source install/setup.sh
    colcon --log-base /dev/null test \
        --ctest-args tests --symlink-install \
        --executor parallel --parallel-workers $$(nproc) \
        --event-handlers console_cohesion+

.PHONY: build
# packages="first_pkg second_pkg third_pkg..."
build:
    source ${ROS_ROOT}/setup.sh
    colcon --log-base /dev/null build \
        --base-paths packages \
        --symlink-install \
        --cmake-args ${CMAKE_ARGS} \
        --packages-up-to $(packages)

.PHONY: test
# packages="first_pkg second_pkg third_pkg..."
test:
    source ${ROS_ROOT}/setup.sh
    source install/setup.sh
    colcon --log-base /dev/null test --ctest-args tests --symlink-install \
        --executor parallel --parallel-workers $$(nproc) \
        --event-handlers console_cohesion+ --packages-select $(packages)

.PHONY: lint-all
# args="-fix ..."
lint-all:
    run-clang-tidy -p=build $(args) $(FILES_TO_LINT)

.PHONY: lint
# args="-fix ..."
# files="first_file second_file third_file..."
lint:
    run-clang-tidy -p=build $(args) $(files)

.PHONY: clean
clean:
    rm -rf build install