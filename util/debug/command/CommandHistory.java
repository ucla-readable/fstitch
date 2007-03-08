/**
 * An interface to support command history expansion.
 * */

package command;

public interface CommandHistory
{
	/**
	 * Expand a line using the command history.
	 *
	 * If a non-null string is returned, it is the expansion of the given
	 * line. Otherwise there was no expansion necessary.
	 *
	 * @param line The command line to expand.
	 * @return The expanded command line, or null if no expansion was necessary.
	 * @throws HistoryException If a bad history request was found.
	 * */
	public String expandLine(String line) throws HistoryException;
	
	/**
	 * Add a line to the command history.
	 *
	 * Depending on the implementation, old history lines may be silently
	 * forgotten when new history lines are added.
	 *
	 * @param line The command line to add.
	 * */
	public void addLine(String line);
	
	/**
	 * Clears the history.
	 * */
	public void clearHistory();
}
