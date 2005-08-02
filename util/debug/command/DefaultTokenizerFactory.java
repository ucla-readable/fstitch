/**
 * The default tokenizer used by CommandInterpreter.
 *
 * This tokenizer is just a wrapper for StringTokenizer.
 * */

package command;

import java.util.*;

public class DefaultTokenizerFactory implements TokenizerFactory
{
	private class DefaultCommandTokenizer implements CommandTokenizer
	{
		private StringTokenizer tokenizer;
		
		public DefaultCommandTokenizer(String string)
		{
			tokenizer = new StringTokenizer(string);
		}
		
		public boolean hasMoreTokens()
		{
			return tokenizer.hasMoreTokens();
		}
		
		public int countTokens()
		{
			return tokenizer.countTokens();
		}
		
		public String nextToken() throws SyntaxErrorException
		{
			return tokenizer.nextToken();
		}
	}
	
	public CommandTokenizer createTokenizer(String string)
	{
		return new DefaultCommandTokenizer(string);
	}
}
