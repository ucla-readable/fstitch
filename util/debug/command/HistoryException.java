/**
 * An exception thrown when a bad history request is found.
 * */

package command;

public class HistoryException extends CommandException
{
	public HistoryException()
	{
		super();
	}
	
	public HistoryException(String error)
	{
		super(error);
	}
}
