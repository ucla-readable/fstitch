/**
 * An interface for command line tokenizers.
 *
 * Since different programs may have different CLI semantics, it may be
 * necessary to change the way the command line is broken down into tokens.
 *
 * By implementing this interface, the behavior of CommandInterpreter can be
 * tuned to match the needs of the program.
 * */

package command;

public interface CommandTokenizer
{
	public boolean hasMoreTokens();
	
	public int countTokens();
	
	public String nextToken() throws SyntaxErrorException;
}
