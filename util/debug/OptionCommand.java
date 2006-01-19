import java.util.*;

import command.*;

public class OptionCommand implements Command
{
	public String getName()
	{
		return "option";
	}
	
	public String getHelp()
	{
		return "Get or set rendering options: freelist, grouping.";
	}

	/** Primary grouping color. */
	protected final static String color1 = "red";
	/** Secondary grouping color. */
	protected final static String color2 = "gold";
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			if(args.length > 0)
			{
				if("freelist".equals(args[0]))
				{
					String now = "";
					if(args.length != 1)
					{
						if("on".equals(args[1]))
							dbg.setRenderFree(true);
						else if("off".equals(args[1]))
							dbg.setRenderFree(false);
						else
						{
							System.out.println("Invalid setting: " + args[1]);
							return data;
						}
						now = "now ";
					}
					System.out.println("Free list rendering is " + now + (dbg.getRenderFree() ? "on" : "off"));
				}
				else if("grouping".equals(args[0]))
				{
					String now = "";
					if(args.length != 1)
					{
						String groupingType = args[1];
						GrouperFactory grouperFactory;
						GrouperFactory noneFactory = NoneGrouper.Factory.getFactory();
						if("off".equals(groupingType))
							grouperFactory = NoneGrouper.Factory.getFactory();
						else if("block".equals(groupingType))
							grouperFactory = new BlockGrouper.Factory(noneFactory, color1);
						else if("owner".equals(groupingType))
							grouperFactory = new OwnerGrouper.Factory(noneFactory, color1, dbg);
						else if("block-owner".equals(groupingType))
							grouperFactory = new BlockGrouper.Factory(new OwnerGrouper.Factory(noneFactory, color1, dbg), color2);
						else if("owner-block".equals(groupingType))
							grouperFactory = new OwnerGrouper.Factory(new BlockGrouper.Factory(noneFactory, color1), color2, dbg);
						else {
							System.out.println("Invalid setting: " + groupingType);
							return data;
						}

						dbg.setGrouperFactory(grouperFactory);
						now = "now ";
					}
					else
						System.out.println("Available groupings: off, block, owner, block-owner, owner-block.");
					System.out.println("Change descriptor grouping is " + now + dbg.getGrouperFactory());
				}
				else
					System.out.println("Invalid option: " + args[0]);
			}
			else
				System.out.println("Need an option to get or set.");
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
