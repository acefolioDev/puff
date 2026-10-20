.PHONY: all test integration ui-test check format rpm deb clean
all:
	bash scripts/build-native.sh
test:
	bash scripts/test-native.sh
integration:
	bash scripts/test-native-postgres.sh
ui-test: all
	bash scripts/test-native-ui.sh
check:
	clang-format --dry-run --Werror app/*.cpp app/*.hpp tests/native/*.cpp
format:
	clang-format -i app/*.cpp app/*.hpp tests/native/*.cpp
rpm: all
	bash scripts/package-rpm.sh
deb:
	bash scripts/package-deb.sh
clean:
	rm -rf dist/objects dist/puff dist/puff-tests
