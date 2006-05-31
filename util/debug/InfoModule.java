import java.io.IOException;

public class InfoModule extends Module
{
	public InfoModule(CountingDataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_INFO);
		
		addFactory(InfoMark.getFactory(input));
		addFactory(InfoBdName.getFactory(input));
		addFactory(InfoBdescNumber.getFactory(input));
		addFactory(InfoChdescLabel.getFactory(input));
	}
}
