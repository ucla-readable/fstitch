/**
 * A command interpreter class to simplify CLI programs.
 *
 * It supports dynamic command updating, help blurbs, parameter passing, and
 * customizable command line tokenizing.
 * */

package command;

import java.util.*;
import java.io.*;

public class CommandInterpreter
{
	private Vector commands;
	private boolean quit;
	private TokenizerFactory factory;
	private CommandHistory history;
	
	private Command findCommand(String name, boolean prefix) throws NoSuchCommandException
	{
		Iterator iterator = commands.iterator();
		Command best = null;
		int length = 0;
		while(iterator.hasNext())
		{
			Command command = (Command) iterator.next();
			String match = command.getName();
			if(name.equals(match))
				return command;
			if(match.startsWith(name))
			{
				int size = name.length();
				if(size > length)
				{
					best = command;
					length = size;
				}
				else if(size == length)
					best = null;
			}
		}
		if(prefix && best != null)
			return best;
		throw new NoSuchCommandException();
	}
	
	/**
	 * The default constructor.
	 *
	 * The default command line tokenizer is used.
	 * */
	public CommandInterpreter()
	{
		this(new DefaultTokenizerFactory());
	}
	
	/**
	 * A constructor taking a TokenizerFactory.
	 *
	 * @param f The TokenizerFactory to use when creating a tokenizer for each command line.
	 * */
	public CommandInterpreter(TokenizerFactory f)
	{
		commands = new Vector();
		quit = false;
		factory = f;
	}
	
	/**
	 * A constructor taking a CommandHistory.
	 *
	 * @param h The CommandHistory object to use for expanding past commands.
	 * */
	public CommandInterpreter(CommandHistory h)
	{
		this();
		history = h;
	}
	
	/**
	 * A constructor taking a TokenizerFactory and a CommandHistory.
	 *
	 * @param f The TokenizerFactory to use when creating a tokenizer for each command line.
	 * @param h The CommandHistory object to use for expanding past commands.
	 * */
	public CommandInterpreter(TokenizerFactory f, CommandHistory h)
	{
		this(f);
		history = h;
	}
	
	/**
	 * Adds a new command to this CommandInterpreter.
	 *
	 * The newly added command is effective immediately.
	 *
	 * @param command The command to be added.
	 * @throws DuplicateCommandException If the new command has the same name as another.
	 * */
	public void addCommand(Command command) throws DuplicateCommandException
	{
		try {
			findCommand(command.getName(), false);
			throw new DuplicateCommandException();
		}
		catch(NoSuchCommandException e)
		{
			commands.add(command);
		}
	}
	
	/**
	 * Removes an existing command from this CommandInterpreter.
	 *
	 * The removal is effective immediately.
	 *
	 * @param name The name of the command to be removed.
	 * @throws NoSuchCommandException If the command to be removed does not exist.
	 * */
	public void removeCommand(String name) throws NoSuchCommandException
	{
		commands.remove(findCommand(name, false));
	}
	
	/**
	 * Returns an iterator of the commands in this CommandInterpreter.
	 *
	 * @return The iterator of the commands.
	 * */
	public Iterator getCommands()
	{
		return commands.iterator();
	}
	
	/**
	 * Returns an iterator of the command names in this CommandInterpreter.
	 *
	 * @return The iterator of the command names.
	 * */
	public Iterator getCommandNames()
	{
		Iterator iterator = commands.iterator();
		Vector names = new Vector();
		while(iterator.hasNext())
			names.add(((Command) iterator.next()).getName());
		return names.iterator();
	}
	
	/**
	 * Returns the CommandHistory in use by this CommandInterpreter.
	 *
	 * @return The CommandHistory being used.
	 * */
	public CommandHistory getCommandHistory()
	{
		return history;
	}
	
	/**
	 * Runs a command line.
	 *
	 * This is the method to be called from a main control loop.
	 * It takes a command line, parses it using a StringTokenizer, and
	 * searches for a matching command name.
	 *
	 * If found, the rest of the command line is broken up into arguments
	 * and passed to the command's runCommand() method along with any state
	 * data provided and 'this' to allow commands like 'help' to ask the
	 * interpreter for a list of commands.
	 *
	 * The return value of the command (its return state data) is returned.
	 *
	 * @param line The command line to be run.
	 * @param data The state data to be passed to the command.
	 * @param prefix Whether or not to allow unique prefixes of command names.
	 * @return The return state data from the command, or the input object if the line was empty.
	 * @throws CommandException If any sort of CommandException occurs while parsing the command line or executing the command.
	 * */
	public Object runCommandLine(String line, Object data, boolean prefix) throws CommandException
	{
		CommandTokenizer tokenizer;
		if(history != null)
		{
			String expand = history.expandLine(line);
			if(expand != null)
				line = expand;
		}
		tokenizer = factory.createTokenizer(line);
		if(tokenizer.hasMoreTokens())
		{
			String name = tokenizer.nextToken().toLowerCase();
			Command command = findCommand(name, prefix);
			String args[] = new String[tokenizer.countTokens()];
			for(int i = 0; tokenizer.hasMoreTokens(); i++)
				args[i] = tokenizer.nextToken();
			if(history != null)
				history.addLine(line);
			return command.runCommand(args, data, this);
		}
		return data;
	}
	
	/**
	 * Runs command lines read from standard input.
	 *
	 * This is a utility method that implements a simple but likely common
	 * main control loop. Instead of calling runCommandLine(), you can call
	 * this method and it will read lines from standard input and call
	 * runCommandLine() for each. Each command is passed the return value
	 * from the previous command, and the return value from the last command
	 * is returned.
	 *
	 * @param prompt The prompt to display on standard output before reading each line.
	 * @param data The state data to be passed to the first command.
	 * @param prefix Whether or not to allow unique prefixes of command names.
	 * @return The return state data from the last command.
	 * @throws CommandException If any sort of CommandException occurs while parsing an input line or executing a command.
	 * @throws IOException If any sort of I/O error occurs while reading standard input.
	 * */
	public Object runStdinCommands(String prompt, Object data, boolean prefix) throws CommandException, IOException
	{
		BufferedReader stdin = new BufferedReader(new InputStreamReader(System.in));
		while(!quit)
		{
			String input;
			System.out.print(prompt);
			input = stdin.readLine();
			if(input == null)
			{
				System.out.println();
				break;
			}
			try {
				data = runCommandLine(input, data, prefix);
			}
			catch(NoSuchCommandException e)
			{
				System.out.println("No such command.");
			}
			catch(HistoryException e)
			{
				System.out.println("Bad history expansion.");
			}
		}
		return data;
	}
	
	/**
	 * Gets the state of the 'quit' flag.
	 *
	 * The quit flag is an easy way to implement simple CLI 'quit' commands.
	 * By using the default QuitCommand, typing 'quit' at the command line
	 * will set this flag to true, which can be tested in the main loop.
	 *
	 * @return The state of the 'quit' flag.
	 * */
	public boolean getQuitFlag()
	{
		return quit;
	}
	
	/**
	 * Sets the state of the 'quit' flag.
	 *
	 * @param state The new state for the 'quit' flag.
	 * */
	public void setQuitFlag(boolean state)
	{
		quit = state;
	}
}
