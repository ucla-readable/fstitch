/**
 * A default help command.
 *
 * This command will give a list of the available commands and their help when
 * 'help' is typed.
 * */

package command;

import java.util.*;

public class HelpCommand implements Command
{
	private String name, help;
	
	/**
	 * Default constructor.
	 * */
	public HelpCommand()
	{
		this("Displays help.");
	}
	
	/**
	 * Constructor taking the help text only.
	 * */
	public HelpCommand(String h)
	{
		this("help", h);
	}
	
	/**
	 * Constructor taking the name of the command and its help text.
	 * */
	public HelpCommand(String n, String h)
	{
		name = n;
		help = h;
	}
	
	public String getName()
	{
		return name;
	}
	
	public String getHelp()
	{
		return help;
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Iterator iterator = interpreter.getCommands();
		System.out.println("Commands:");
		while(iterator.hasNext())
		{
			Command command = (Command) iterator.next();
			System.out.println("\t" + command.getName());
			System.out.println("\t\t" + command.getHelp());
		}
		return data;
	}
}
