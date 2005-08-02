/**
 * A generic exception thrown by the command system.
 *
 * All exceptions thrown by commands should extend this exception.
 * */

package command;

public class CommandException extends Exception
{
	public CommandException()
	{
		super();
	}
	
	public CommandException(String error)
	{
		super(error);
	}
}
