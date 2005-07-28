import java.io.DataInput;
import java.io.IOException;

class ChdescApplyFactory extends ModuleOpcodeFactory
{
	public ChdescApplyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_APPLY, "KDB_CHDESC_APPLY");
		addParameter("chdesc", 4);
	}
	
	public ChdescApply readChdescApply() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescApply();
	}
}

public class ChdescApply extends Opcode
{
	public ChdescApply(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescApplyFactory(input);
	}
}
