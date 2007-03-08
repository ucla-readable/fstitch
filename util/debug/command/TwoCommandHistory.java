/**
 * A simple two-command history.
 *
 * The token ! executes the most recent command, while !! executes the command
 * before that.
 * */

package command;

public class TwoCommandHistory implements CommandHistory
{
	private String lastCommand, secondCommand;
	
	public String expandLine(String line) throws HistoryException
	{
		if("!".equals(line))
		{
			if(lastCommand == null)
				throw new HistoryException();
			return lastCommand;
		}
		if("!!".equals(line))
		{
			if(secondCommand == null)
				throw new HistoryException();
			return secondCommand;
		}
		return null;
	}
	
	public void addLine(String line)
	{
		secondCommand = lastCommand;
		lastCommand = line;
	}
	
	public void clearHistory()
	{
		lastCommand = null;
		secondCommand = null;
	}
}
