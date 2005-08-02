/**
 * An exception thrown when a new command would have the same name as another.
 * */

package command;

public class DuplicateCommandException extends CommandException
{
	public DuplicateCommandException()
	{
		super();
	}
	
	public DuplicateCommandException(String error)
	{
		super(error);
	}
}
