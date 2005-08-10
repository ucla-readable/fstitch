# This simple Makefile just runs ant when necessary, in case you run make instead of ant.
# It actually saves time if everything is up to date, because ant is not run in that case.

SOURCES=$(wildcard *.java)
COMMAND=$(wildcard command/*.java)

kdb.jar: $(SOURCES) $(COMMAND)
	@ant
