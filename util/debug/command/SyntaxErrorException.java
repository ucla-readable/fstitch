/**
 * An exception thrown when the command line contains a syntax error.
 *
 * Examples are mismatched quotes. The default tokenizer does not throw this
 * exception.
 * */

package command;

public class SyntaxErrorException extends CommandException
{
	public SyntaxErrorException()
	{
		super();
	}
	
	public SyntaxErrorException(String error)
	{
		super(error);
	}
}
