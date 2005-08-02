/**
 * An exception thrown when no command is available with the requested name.
 * */

package command;

public class NoSuchCommandException extends CommandException
{
	public NoSuchCommandException()
	{
		super();
	}
	
	public NoSuchCommandException(String error)
	{
		super(error);
	}
}
