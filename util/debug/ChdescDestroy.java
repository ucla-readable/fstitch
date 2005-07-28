import java.io.DataInput;
import java.io.IOException;

public class ChdescDestroy extends Opcode
{
	public ChdescDestroy(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DESTROY, "KDB_CHDESC_DESTROY", ChdescDestroy.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
