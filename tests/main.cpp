#define NO_TEST_MAIN
#include "3rdparty/catch2/test_main.hpp"

#include <QCoreApplication>

int main(int argc, char* argv[])
{
	QCoreApplication app(argc, argv);
	return runCatchSession(argc, argv);
}
