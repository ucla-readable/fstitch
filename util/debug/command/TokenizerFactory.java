/**
 * An interface for command line tokenizer factories.
 *
 * The CommandInterpreter needs to be able to construct a new CommandTokenizer
 * for each command, so it must be passed a TokenizerFactory to create them.
 * */

package command;

public interface TokenizerFactory
{
	public CommandTokenizer createTokenizer(String string);
}
